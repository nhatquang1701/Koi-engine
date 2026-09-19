"""Koi NNUE Studio - a small tkinter front end for training Koi networks.

Press the launcher (``Koi NNUE Studio.cmd``) to open the GUI, or use the
headless entry points so the same pipeline can run from a terminal, a script or
CI::

    python tools/nnue/koi_nnue_studio.py --list-backends
    python tools/nnue/koi_nnue_studio.py --dry-run --preset quick
    python tools/nnue/koi_nnue_studio.py --run standard --detach
    python tools/nnue/koi_nnue_studio.py --selftest --rows 2000 --epochs 1

Every run stores its configuration, command line, log and artifacts under
``artifacts/training/runs/<stamp>-<kind>-<backend>/`` so results are
reproducible and comparable.
"""

from __future__ import annotations

import argparse
import os
import queue
import subprocess
import sys
import threading
import time
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))

import studio_core as core  # noqa: E402
from backends import available_backends, get_backend  # noqa: E402

APP_TITLE = "Koi NNUE Studio"


# ---------------------------------------------------------------------------
# Headless entry points
# ---------------------------------------------------------------------------


def merged_config(args: argparse.Namespace, preset: str | None) -> dict:
    config = core.default_config()
    if preset:
        config.update(core.presets()[preset])
    config["backend"] = args.backend
    if args.corpus:
        config["corpus"] = str(Path(args.corpus).resolve())
    for name in ("epochs", "batch_size", "learning_rate", "threads", "rows", "net_name"):
        value = getattr(args, name, None)
        if value is not None:
            config[name] = value
    return config


def print_backends() -> int:
    for backend in available_backends():
        if backend.available():
            print(f"{backend.name:8} available    {backend.label}")
        else:
            print(f"{backend.name:8} unavailable  {backend.label} - {backend.unavailable_reason()}")
    return 0


def dry_run(args: argparse.Namespace) -> int:
    config = merged_config(args, args.preset)
    backend = get_backend(config["backend"])
    try:
        command = backend.build_command(core.RUNS_DIR / "<run>", config)
    except RuntimeError as error:
        print(f"{backend.name} cannot build a command yet: {error}", file=sys.stderr)
        return 2
    print(subprocess.list2cmdline(command))
    return 0


def stream_command(command: list[str], run: core.Run, echo: bool = True) -> int:
    """Run synchronously, streaming and parsing progress into the run state."""
    process = subprocess.Popen(
        command,
        cwd=str(core.REPO_ROOT),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert process.stdout is not None
    with open(run.log_path, "w", encoding="utf-8") as log:
        for line in process.stdout:
            log.write(line)
            log.flush()
            if echo:
                print(line, end="", flush=True)
            event = core.parse_progress(line)
            if event:
                run.update_from_log_events([event])
    code = process.wait()
    run.write_state(
        status="completed" if code == 0 else "failed",
        exit_code=code,
        finished=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    )
    return code


def selftest(args: argparse.Namespace) -> int:
    """Train a tiny network end to end and verify the engine can load it."""
    config = merged_config(args, args.preset or "quick")
    config["epochs"] = args.epochs or 1
    config["rows"] = args.rows or 2_000
    config["threads"] = args.threads or 2
    config["batch_size"] = args.batch_size or 1_024
    config["net_name"] = args.net_name or "selftest.nnue"
    backend = get_backend(config["backend"])
    if not backend.available():
        print(f"selftest cannot run: {backend.unavailable_reason()}", file=sys.stderr)
        return 2
    if backend.name == "koi":
        # A tiny hidden layer keeps the wiring selftest fast; real runs use the preset width.
        config["koi_hidden_units"] = min(int(config.get("koi_hidden_units", 1024)), 64)
    run = core.create_run("selftest", config, backend.name)
    command = backend.build_command(run.directory, config)
    core.build_command_file(run, command)
    print(f"[selftest] run directory {run.directory}")
    print(f"[selftest] command {subprocess.list2cmdline(command)}")
    code = stream_command(command, run)
    if code != 0:
        print(f"[selftest] trainer exited with {code}", file=sys.stderr)
        return code
    net = backend.net_path(run.directory, run.config)
    if not net.exists() or net.stat().st_size == 0:
        print(f"[selftest] trainer did not produce {net}", file=sys.stderr)
        return 1
    print(f"[selftest] network {net} ({net.stat().st_size} bytes)")
    if args.no_gate or not core.DEFAULT_BENCH.exists():
        print("[selftest] skipping the engine load check (koi-bench unavailable or --no-gate)")
        return 0
    gate = core.run_gate(net)
    if gate.get("rejected"):
        print(f"[selftest] the engine rejected the network: {gate.get('stderr')}", file=sys.stderr)
        return 1
    print(
        f"[selftest] engine loaded the network; gate matches "
        f"{gate['matches']}/{gate['positions']}"
    )
    return 0


def run_headless(args: argparse.Namespace) -> int:
    preset = args.preset or "standard"
    config = merged_config(args, preset)
    backend = get_backend(config["backend"])
    if not backend.available():
        print(f"backend unavailable: {backend.unavailable_reason()}", file=sys.stderr)
        return 2
    run = core.create_run("train", config, backend.name)
    command = backend.build_command(run.directory, config)
    core.build_command_file(run, command)
    print(f"run directory: {run.directory}")
    if args.detach:
        core.launch_run(run)
        print(f"launched detached; log: {run.log_path}")
        return 0
    return stream_command(command, run)


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------


class StudioApp:
    """The tkinter application; all state lives here and in studio_core."""

    def __init__(self) -> None:
        import tkinter as tk
        import tkinter.ttk as ttk
        from tkinter import filedialog, messagebox

        self.tk = tk
        self.ttk = ttk
        self.filedialog = filedialog
        self.messagebox = messagebox
        self._apply_dpi_awareness()
        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} {core.STUDIO_VERSION}")
        self.settings = core.read_settings()
        self.root.geometry(str(self.settings.get("geometry") or "1000x780"))
        self.root.minsize(820, 620)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        try:
            pixels_per_inch = self.root.winfo_fpixels("1i")
            if pixels_per_inch > 0:
                self.root.tk.call("tk", "scaling", pixels_per_inch / 72.0)
        except Exception:  # noqa: BLE001 - DPI scaling is best-effort
            pass
        self.queue: queue.Queue = queue.Queue()
        self.active_run: core.Run | None = None
        self.log_offset = 0
        self.tail_stop = threading.Event()
        self.busy = False
        self.validation_cancel: core.CancelToken | None = None
        self.runs_refreshing = False
        self._chart_geometry: tuple | None = None
        self.theme_var = tk.StringVar(value=str(self.settings.get("theme") or "light"))

        self.backends = available_backends()
        self.backend_names = [backend.name for backend in self.backends]
        self.preset_names = list(core.presets())
        self._build_style()
        self._build_layout()
        self._apply_theme(self.theme_var.get())
        self._pump()

    # -- construction -------------------------------------------------------

    def _apply_dpi_awareness(self) -> None:
        """Best-effort per-monitor DPI awareness before Tk creates windows."""
        try:
            import ctypes
        except Exception:  # noqa: BLE001 - not on Windows
            return
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:  # noqa: BLE001 - older Windows
            try:
                ctypes.windll.user32.SetProcessDPIAware()
            except Exception:  # noqa: BLE001 - best effort
                pass

    def _build_style(self) -> None:
        self.style = self.ttk.Style(self.root)
        if "vista" in self.style.theme_names():
            self.style.theme_use("vista")
        self.style.configure("Heading.TLabel", font=("Segoe UI", 10, "bold"))

    def _apply_theme(self, theme: str) -> None:
        """Reconfigure ttk styles and text colors for the chosen palette."""
        theme = "dark" if str(theme).lower() == "dark" else "light"
        self.theme_var.set(theme)
        colors = core.palette(theme)
        core.configure_styles(self.style, colors)
        for widget in (getattr(self, "log_text", None), getattr(self, "runs_detail", None)):
            if widget is None:
                continue
            background = colors["chart_bg"] if widget is self.log_text else colors["surface"]
            foreground = colors["text"] if widget is self.runs_detail else colors["text"]
            widget.configure(background=background, foreground=foreground, insertbackground=colors["text"])
        if getattr(self, "chart", None) is not None:
            self.chart.configure(background=colors["chart_bg"], highlightbackground=colors["border"])
            self._draw_chart()
        if getattr(self, "validation_text", None) is not None:
            self.validation_text.configure(
                background=colors["surface"], foreground=colors["text"], insertbackground=colors["text"]
            )

    def _scrollable_tab(self, notebook, text: str):
        """Create a tab whose content scrolls when the window is short."""
        tk = self.tk
        outer = tk.ttk.Frame(notebook)
        notebook.add(outer, text=text)
        canvas = tk.Canvas(outer, highlightthickness=0)
        scrollbar = tk.ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        inner = tk.ttk.Frame(canvas, padding=12)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")
        inner.bind("<Configure>", lambda _event: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.bind("<Configure>", lambda event: canvas.itemconfigure(window, width=event.width))

        def on_wheel(event) -> None:
            canvas.yview_scroll(-1 if event.delta > 0 else 1, "units")

        canvas.bind("<Enter>", lambda _event: canvas.bind_all("<MouseWheel>", on_wheel))
        canvas.bind("<Leave>", lambda _event: canvas.unbind_all("<MouseWheel>"))
        return inner

    def _build_layout(self) -> None:
        toolbar = self.ttk.Frame(self.root)
        toolbar.pack(fill="x", padx=8, pady=(8, 0))
        self.ttk.Label(toolbar, text="Theme").pack(side="left")
        theme_box = self.ttk.Combobox(
            toolbar, textvariable=self.theme_var, values=("light", "dark"), state="readonly", width=8
        )
        theme_box.pack(side="left", padx=(4, 0))
        theme_box.bind("<<ComboboxSelected>>", lambda _event: self._apply_theme(self.theme_var.get()))
        self.ttk.Label(
            toolbar,
            text="Ctrl+T train   Ctrl+R refresh runs   F5 validate   Esc stop/cancel",
            style="Muted.TLabel",
        ).pack(side="right")

        notebook = self.ttk.Notebook(self.root)
        notebook.pack(fill="both", expand=True, padx=8, pady=(8, 4))
        self._build_data_tab(notebook)
        self._build_train_tab(notebook)
        self._build_validate_tab(notebook)
        self._build_runs_tab(notebook)
        self.status_var = self.tk.StringVar(value="Ready")
        self.ttk.Label(self.root, textvariable=self.status_var, style="Status.TLabel").pack(
            fill="x", padx=10, pady=(0, 8)
        )
        self.root.bind_all("<Control-t>", lambda _event: self._start_training())
        self.root.bind_all("<Control-r>", lambda _event: self._refresh_runs())
        self.root.bind_all("<F5>", lambda _event: self._start_validation(auto=False))
        self.root.bind_all("<Escape>", lambda _event: self._escape())

    def _escape(self) -> None:
        """Esc stops a training run or cancels a validation."""
        if self.validation_cancel is not None:
            self._cancel_validation()
        else:
            self._stop_training()

    def _build_data_tab(self, notebook) -> None:
        tk = self.tk
        frame = self._scrollable_tab(notebook, "Data")

        tk.ttk.Label(frame, text="Corpus and labeling", style="Heading.TLabel").grid(
            row=0, column=0, columnspan=4, sticky="w", pady=(0, 8)
        )
        tk.ttk.Label(frame, text="Positions file").grid(row=1, column=0, sticky="w")
        self.positions_var = tk.StringVar(value=str(core.DEFAULT_POSITIONS))
        tk.ttk.Entry(frame, textvariable=self.positions_var, width=64).grid(
            row=1, column=1, columnspan=2, sticky="we", padx=6
        )
        tk.ttk.Button(frame, text="Browse", command=lambda: self._browse_file(self.positions_var)).grid(
            row=1, column=3
        )
        tk.ttk.Label(frame, text="Labels file").grid(row=2, column=0, sticky="w", pady=4)
        self.labels_var = tk.StringVar(value=str(core.DEFAULT_CORPUS))
        tk.ttk.Entry(frame, textvariable=self.labels_var, width=64).grid(
            row=2, column=1, columnspan=2, sticky="we", padx=6
        )
        tk.ttk.Button(frame, text="Browse", command=lambda: self._browse_file(self.labels_var)).grid(
            row=2, column=3
        )
        self.data_counts_var = tk.StringVar(value="")
        tk.ttk.Label(frame, textvariable=self.data_counts_var).grid(
            row=3, column=1, columnspan=3, sticky="w", pady=(2, 8)
        )
        tk.ttk.Button(frame, text="Count rows", command=self._count_rows).grid(row=3, column=0, sticky="w")

        options = tk.ttk.LabelFrame(frame, text="Generation options", padding=10)
        options.grid(row=4, column=0, columnspan=4, sticky="we", pady=8)
        self.games_var = tk.StringVar(value="2000")
        self.workers_var = tk.StringVar(value=str(max(1, (os.cpu_count() or 4) - 1)))
        self.game_depth_var = tk.StringVar(value="4")
        self.label_depth_var = tk.StringVar(value="9")
        self.label_limit_var = tk.StringVar(value="0")
        self.resume_var = tk.BooleanVar(value=True)
        for column, (label, variable, width) in enumerate(
            [
                ("Games", self.games_var, 7),
                ("Workers", self.workers_var, 4),
                ("Game depth", self.game_depth_var, 4),
                ("Label depth", self.label_depth_var, 4),
                ("Label limit (0 = all)", self.label_limit_var, 10),
            ]
        ):
            tk.ttk.Label(options, text=label).grid(row=0, column=column * 2, sticky="w", padx=(0, 4))
            tk.ttk.Entry(options, textvariable=variable, width=width).grid(
                row=0, column=column * 2 + 1, sticky="w", padx=(0, 10)
            )
        tk.ttk.Checkbutton(
            options, text="Resume labeling", variable=self.resume_var
        ).grid(row=1, column=0, columnspan=2, sticky="w", pady=(8, 0))
        tk.ttk.Button(options, text="Generate positions", command=self._start_generate).grid(
            row=1, column=2, columnspan=2, sticky="w", pady=(8, 0)
        )
        tk.ttk.Button(options, text="Label / continue", command=self._start_label).grid(
            row=1, column=4, columnspan=2, sticky="w", pady=(8, 0)
        )
        tk.ttk.Label(
            frame,
            wraplength=850,
            foreground="#555555",
            text=(
                "Generation uses the bundled Stockfish 19 and runs detached, so it continues while "
                "the studio is closed. Labeling roughly processes 200-250 positions per second on "
                "this machine at the configured label depth (the field defaults to 9)."
            ),
        ).grid(row=5, column=0, columnspan=4, sticky="w")
        frame.columnconfigure(1, weight=1)

    def _build_train_tab(self, notebook) -> None:
        tk = self.tk
        frame = self._scrollable_tab(notebook, "Train")

        tk.ttk.Label(frame, text="Run configuration", style="Heading.TLabel").grid(
            row=0, column=0, columnspan=6, sticky="w", pady=(0, 6)
        )
        self.backend_var = tk.StringVar(value=self.backend_names[0])
        self.preset_var = tk.StringVar(value="standard")
        tk.ttk.Label(frame, text="Backend").grid(row=1, column=0, sticky="w")
        backend_box = tk.ttk.Combobox(
            frame, textvariable=self.backend_var, values=self.backend_names, state="readonly", width=16
        )
        backend_box.grid(row=1, column=1, sticky="w", padx=(4, 12))
        backend_box.bind("<<ComboboxSelected>>", lambda _event: self._backend_changed())
        tk.ttk.Label(frame, text="Preset").grid(row=1, column=2, sticky="w")
        preset_box = tk.ttk.Combobox(
            frame, textvariable=self.preset_var, values=self.preset_names, state="readonly", width=12
        )
        preset_box.grid(row=1, column=3, sticky="w", padx=(4, 12))
        preset_box.bind("<<ComboboxSelected>>", lambda _event: self._apply_preset())
        self.backend_note_var = tk.StringVar(value="")
        tk.ttk.Label(frame, textvariable=self.backend_note_var, foreground="#a05000").grid(
            row=2, column=0, columnspan=6, sticky="w", pady=(2, 6)
        )

        fields = tk.ttk.Frame(frame)
        fields.grid(row=3, column=0, columnspan=6, sticky="we")
        self.epochs_var = tk.StringVar(value="10")
        self.batch_var = tk.StringVar(value="4096")
        self.lr_var = tk.StringVar(value="0.002")
        self.threads_var = tk.StringVar(value=str(max(1, (os.cpu_count() or 4) - 1)))
        self.rows_var = tk.StringVar(value="0")
        self.val_fraction_var = tk.StringVar(value="0.05")
        self.net_name_var = tk.StringVar(value="koi.nnue")
        self.games_ab_var = tk.StringVar(value="20")
        self.nodes_ab_var = tk.StringVar(value="20000")
        for index, (label, variable, width) in enumerate(
            [
                ("Epochs", self.epochs_var, 6),
                ("Batch size", self.batch_var, 8),
                ("Learning rate", self.lr_var, 8),
                ("Threads", self.threads_var, 4),
                ("Row cap (0 = all)", self.rows_var, 9),
                ("Val fraction", self.val_fraction_var, 6),
                ("Network name", self.net_name_var, 14),
            ]
        ):
            column = (index % 4) * 2
            row = index // 4
            tk.ttk.Label(fields, text=label).grid(row=row, column=column, sticky="w", padx=(0, 4), pady=2)
            tk.ttk.Entry(fields, textvariable=variable, width=width).grid(
                row=row, column=column + 1, sticky="w", padx=(0, 12), pady=2
            )

        buttons = tk.ttk.Frame(frame)
        buttons.grid(row=4, column=0, columnspan=6, sticky="we", pady=(8, 4))
        self.start_button = tk.ttk.Button(buttons, text="Start training", command=self._start_training)
        self.start_button.pack(side="left")
        self.stop_button = tk.ttk.Button(buttons, text="Stop", command=self._stop_training, state="disabled")
        self.stop_button.pack(side="left", padx=6)
        self.open_run_button = tk.ttk.Button(
            buttons, text="Open run folder", command=self._open_active_run, state="disabled"
        )
        self.open_run_button.pack(side="left", padx=6)
        self.auto_validate_var = tk.BooleanVar(value=True)
        tk.ttk.Checkbutton(
            buttons, text="Validate when finished", variable=self.auto_validate_var
        ).pack(side="left", padx=12)
        self.auto_adopt_var = tk.BooleanVar(value=bool(self.settings.get("auto_adopt", False)))
        tk.ttk.Checkbutton(buttons, text="Auto-adopt", variable=self.auto_adopt_var).pack(
            side="left", padx=(0, 12)
        )
        tk.ttk.Label(buttons, text="A/B games").pack(side="left")
        tk.ttk.Entry(buttons, textvariable=self.games_ab_var, width=5).pack(side="left", padx=(4, 10))
        tk.ttk.Label(buttons, text="A/B nodes").pack(side="left")
        tk.ttk.Entry(buttons, textvariable=self.nodes_ab_var, width=8).pack(side="left", padx=4)

        self.progress = tk.ttk.Progressbar(frame, mode="determinate", maximum=10)
        self.progress.grid(row=5, column=0, columnspan=6, sticky="we", pady=(4, 2))
        self.progress_var = tk.StringVar(value="idle")
        tk.ttk.Label(frame, textvariable=self.progress_var).grid(row=6, column=0, columnspan=6, sticky="w")

        chart_header = tk.ttk.Frame(frame)
        chart_header.grid(row=7, column=0, columnspan=6, sticky="we", pady=(8, 0))
        tk.ttk.Label(chart_header, text="Training progress", style="Heading.TLabel").pack(side="left")
        tk.ttk.Button(chart_header, text="Export chart", command=self._export_chart).pack(side="right")
        self.chart = tk.Canvas(frame, height=130, background="#fdfdfd", highlightthickness=1, highlightbackground="#cccccc")
        self.chart.grid(row=8, column=0, columnspan=6, sticky="we", pady=(2, 8))
        self.chart.bind("<Configure>", lambda _event: self._draw_chart())
        self.chart.bind("<Motion>", self._on_chart_motion)

        log_header = tk.ttk.Frame(frame)
        log_header.grid(row=9, column=0, columnspan=6, sticky="we")
        tk.ttk.Label(log_header, text="Log", style="Heading.TLabel").pack(side="left")
        tk.ttk.Label(log_header, text="Filter").pack(side="left", padx=(12, 4))
        self.log_filter_var = tk.StringVar(value="")
        tk.ttk.Entry(log_header, textvariable=self.log_filter_var, width=24).pack(side="left")
        self.log_filter_var.trace_add("write", lambda *_args: self._render_log())
        self.log_errors_var = tk.BooleanVar(value=False)
        tk.ttk.Checkbutton(
            log_header, text="Errors only", variable=self.log_errors_var, command=self._render_log
        ).pack(side="left", padx=8)
        self.log_text = tk.Text(frame, height=14, wrap="none", background="#111111", foreground="#d8d8d8")
        self.log_text.grid(row=10, column=0, columnspan=6, sticky="nsew")
        scrollbar = tk.ttk.Scrollbar(frame, command=self.log_text.yview)
        scrollbar.grid(row=10, column=6, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)
        frame.rowconfigure(10, weight=1)
        frame.columnconfigure(5, weight=1)
        self.chart_progress: dict = {}
        self.log_lines: list[str] = []
        self._backend_changed()

    def _build_validate_tab(self, notebook) -> None:
        tk = self.tk
        frame = self._scrollable_tab(notebook, "Validate and install")

        tk.ttk.Label(frame, text="Network", style="Heading.TLabel").grid(row=0, column=0, sticky="w")
        self.validate_net_var = tk.StringVar(value="")
        tk.ttk.Entry(frame, textvariable=self.validate_net_var, width=70).grid(
            row=1, column=0, columnspan=3, sticky="we", padx=(0, 6)
        )
        tk.ttk.Button(frame, text="Browse", command=lambda: self._browse_file(self.validate_net_var)).grid(
            row=1, column=3
        )
        tk.ttk.Button(frame, text="Use latest run", command=self._use_latest_net).grid(row=1, column=4, padx=4)

        actions = tk.ttk.LabelFrame(frame, text="Checks", padding=10)
        actions.grid(row=2, column=0, columnspan=5, sticky="we", pady=10)
        tk.ttk.Button(actions, text="Run 64-position gate", command=self._run_gate_only).grid(
            row=0, column=0, sticky="w"
        )
        tk.ttk.Button(actions, text="Run both checks", command=lambda: self._start_validation(auto=False)).grid(
            row=0, column=1, sticky="w", padx=8
        )
        tk.ttk.Label(actions, text="A/B games").grid(row=0, column=2, padx=(16, 4))
        tk.ttk.Entry(actions, textvariable=self.games_ab_var, width=5).grid(row=0, column=3)
        tk.ttk.Label(actions, text="A/B nodes").grid(row=0, column=4, padx=(12, 4))
        tk.ttk.Entry(actions, textvariable=self.nodes_ab_var, width=8).grid(row=0, column=5)
        self.cancel_validation_button = tk.ttk.Button(
            actions, text="Cancel", command=self._cancel_validation, state="disabled"
        )
        self.cancel_validation_button.grid(row=0, column=6, sticky="w", padx=(12, 0))

        self.validation_text = tk.Text(frame, height=16, wrap="word")
        self.validation_text.grid(row=3, column=0, columnspan=5, sticky="nsew")
        frame.rowconfigure(3, weight=1)

        install = tk.ttk.LabelFrame(frame, text="Install", padding=10)
        install.grid(row=4, column=0, columnspan=5, sticky="we", pady=10)
        self.engine_dir_var = tk.StringVar(
            value=str(self.settings.get("engine_directory") or core.DEFAULT_BUILD_DIR)
        )
        tk.ttk.Label(install, text="Engine directory").grid(row=0, column=0, sticky="w")
        tk.ttk.Entry(install, textvariable=self.engine_dir_var, width=60).grid(
            row=0, column=1, columnspan=3, sticky="we", padx=6
        )
        self.install_button = tk.ttk.Button(
            install, text="Install as koi.nnue", command=self._install_net, state="disabled"
        )
        self.install_button.grid(row=1, column=1, sticky="w", pady=(8, 0))
        tk.ttk.Button(install, text="Revert last adoption", command=self._revert_adoption).grid(
            row=1, column=2, sticky="w", padx=8, pady=(8, 0)
        )
        tk.ttk.Label(
            install,
            text="Installing copies the network beside the engine (backing up any previous koi.nnue).",
            foreground="#555555",
        ).grid(row=2, column=0, columnspan=4, sticky="w", pady=(6, 0))
        frame.columnconfigure(0, weight=1)

    def _build_runs_tab(self, notebook) -> None:
        tk = self.tk
        frame = self._scrollable_tab(notebook, "Runs")

        filters = tk.ttk.Frame(frame)
        filters.grid(row=0, column=0, columnspan=6, sticky="we")
        tk.ttk.Label(filters, text="Filter").pack(side="left")
        self.runs_filter_var = tk.StringVar(value="")
        tk.ttk.Entry(filters, textvariable=self.runs_filter_var, width=28).pack(side="left", padx=(4, 0))
        self.runs_filter_var.trace_add("write", lambda *_args: self._refresh_runs())

        columns = ("run", "kind", "status", "val_mae", "duration", "net")
        self.runs_tree = tk.ttk.Treeview(frame, columns=columns, show="headings", height=14)
        for column, heading, width in [
            ("run", "Run", 240),
            ("kind", "Kind", 70),
            ("status", "Status", 90),
            ("val_mae", "Val MAE (cp)", 100),
            ("duration", "Duration", 90),
            ("net", "Network", 220),
        ]:
            self.runs_tree.heading(column, text=heading, command=lambda name=column: self._sort_runs(name))
            self.runs_tree.column(column, width=width, anchor="w")
        self.runs_tree.grid(row=1, column=0, columnspan=6, sticky="nsew")
        self.runs_tree.bind("<<TreeviewSelect>>", lambda _event: self._show_run_detail())
        frame.rowconfigure(1, weight=1)

        buttons = tk.ttk.Frame(frame)
        buttons.grid(row=2, column=0, columnspan=6, sticky="we", pady=8)
        tk.ttk.Button(buttons, text="Refresh", command=self._refresh_runs).pack(side="left")
        tk.ttk.Button(buttons, text="Attach to selected", command=self._attach_selected).pack(side="left", padx=4)
        tk.ttk.Button(buttons, text="Use network", command=self._use_selected_net).pack(side="left", padx=4)
        tk.ttk.Button(buttons, text="Open folder", command=self._open_selected_run).pack(side="left", padx=4)
        tk.ttk.Button(buttons, text="Stop selected", command=self._stop_selected).pack(side="left", padx=4)
        tk.ttk.Button(buttons, text="Adopt best run", command=self._adopt_best_run).pack(
            side="left", padx=4
        )

        self.runs_detail = tk.Text(frame, height=9, wrap="word", background="#f7f7f7", foreground="#333333")
        self.runs_detail.grid(row=3, column=0, columnspan=6, sticky="nsew")
        self.runs_detail.configure(state="disabled")
        self.runs_sort = ("run", True)

    # -- helpers ------------------------------------------------------------

    def _run_backend(self, run: core.Run):
        """Backend for a run, or None for data runs that have no trainer."""
        try:
            return get_backend(run.state.get("backend", "torch"))
        except KeyError:
            return None

    def _browse_file(self, variable) -> None:
        path =         self.filedialog.askopenfilename(initialdir=str(core.REPO_ROOT))
        if path:
            variable.set(path)

    def _backend_changed(self) -> None:
        backend = get_backend(self.backend_var.get())
        self.backend_note_var.set("" if backend.available() else f"unavailable: {backend.unavailable_reason()}")
        self.start_button.configure(state="normal" if backend.available() else "disabled")

    def _apply_preset(self) -> None:
        preset = core.presets()[self.preset_var.get()]
        self.epochs_var.set(str(preset["epochs"]))
        self.batch_var.set(str(preset["batch_size"]))
        self.lr_var.set(str(preset["learning_rate"]))
        self.rows_var.set(str(preset["rows"]))

    def _count_rows(self) -> None:
        positions = self.positions_var.get()
        labels = self.labels_var.get()
        self.status_var.set("counting corpus rows")

        def worker() -> None:
            self.queue.put(("counts", (core.count_rows(positions), core.count_rows(labels))))

        threading.Thread(target=worker, daemon=True).start()

    def _open_path(self, path: Path) -> None:
        if path.exists():
            self.messagebox.showinfo(APP_TITLE, f"Opening {path}")
            subprocess.Popen(["explorer", str(path)])

    def _collect_config(self) -> dict:
        config = core.default_config()
        config["backend"] = self.backend_var.get()
        config["corpus"] = str(Path(self.labels_var.get()))
        config["epochs"] = core.parse_int(self.epochs_var.get(), "Epochs", 1)
        config["batch_size"] = core.parse_int(self.batch_var.get(), "Batch size", 1)
        config["learning_rate"] = core.parse_float(self.lr_var.get(), "Learning rate", 0.0)
        config["threads"] = core.parse_int(self.threads_var.get(), "Threads", 1)
        config["rows"] = core.parse_int(self.rows_var.get(), "Row cap", 0)
        config["val_fraction"] = core.parse_float(
            self.val_fraction_var.get(), "Val fraction", 0.0, 1.0
        )
        config["net_name"] = core.network_file_name({"net_name": self.net_name_var.get()})
        return config

    # -- training -----------------------------------------------------------

    def _start_training(self) -> None:
        try:
            config = self._collect_config()
            backend = get_backend(config["backend"])
            if not backend.available():
                raise RuntimeError(backend.unavailable_reason())
            if core.count_rows(config["corpus"]) == 0:
                raise RuntimeError(f"corpus has no rows: {config['corpus']}")
            run = core.create_run("train", config, backend.name)
            command = backend.build_command(run.directory, config)
            core.build_command_file(run, command)
            core.launch_run(run)
        except Exception as error:  # noqa: BLE001 - reported in the dialog
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        self._attach(run)
        self.status_var.set(f"training in {run.directory.name}")

    def _stop_training(self) -> None:
        if self.active_run:
            core.stop_run(self.active_run)
            self._append_log("\n[studio] stop requested\n")
            self.status_var.set("stopping")

    def _open_active_run(self) -> None:
        if self.active_run:
            self._open_path(self.active_run.directory)

    def _attach(self, run: core.Run) -> None:
        self.tail_stop.set()
        self.active_run = run
        self.log_offset = 0
        self.log_lines = []
        self.log_text.delete("1.0", "end")
        self.chart_progress = dict(run.state.get("progress", {}))
        self._draw_chart()
        self.progress.configure(maximum=max(1, int(run.config.get("epochs", 1))))
        self.progress["value"] = int(self.chart_progress.get("epoch", 0) or 0)
        self.stop_button.configure(state="normal")
        self.open_run_button.configure(state="normal")
        self.tail_stop = threading.Event()
        thread = threading.Thread(target=self._tail_loop, args=(run, self.tail_stop), daemon=True)
        thread.start()
        if self.chart_progress.get("epoch"):
            self.progress_var.set(core.progress_summary(self.chart_progress))
        else:
            self.progress_var.set(f"run {run.directory.name}")

    def _tail_loop(self, run: core.Run, stop: threading.Event) -> None:
        offset = 0
        while not stop.is_set():
            lines, offset = run.tail(offset)
            if lines:
                events = []
                for line in lines:
                    self.queue.put(("log", line + "\n"))
                    event = core.parse_progress(line)
                    if event:
                        events.append(event)
                if events:
                    run.update_from_log_events(events)
                    self.queue.put(("progress", run.state.get("progress", {})))
            state = core.refresh_state(run)
            self.queue.put(("status", state.get("status", "unknown")))
            if not run.is_running():
                self.queue.put(("finished", run))
                return
            time.sleep(0.4)

    # -- validation ---------------------------------------------------------

    def _use_latest_net(self) -> None:
        for run in core.list_runs():
            backend = self._run_backend(run)
            if backend is None:
                continue
            net = backend.net_path(run.directory, run.config)
            if net.exists():
                self.validate_net_var.set(str(net))
                return
        self.messagebox.showinfo(APP_TITLE, "No run has produced a network yet")

    def _gate_report(self, gate: dict) -> str:
        if gate.get("error"):
            return f"gate: failed to run ({gate['error']})"
        if gate.get("rejected"):
            return f"gate: the engine REJECTED the network ({gate.get('stderr', '')[:200]})"
        return (
            f"gate: {gate.get('matches', 0)}/{gate.get('positions', 0)} accepted moves"
            + (f", mismatches: {', '.join(gate.get('failed', [])[:6])}" if gate.get("failed") else "")
        )

    def _ab_report(self, ab: dict) -> str:
        if ab.get("error"):
            return f"A/B match: failed ({ab['error']})"
        if "score" not in ab:
            return "A/B match: no score recorded"
        return (
            f"A/B match ({ab.get('games', 0)} games at {ab.get('nodes', 0)} nodes): "
            f"score {ab['score']:.1f} - {ab.get('wins', 0)}W {ab.get('draws', 0)}D "
            f"{ab.get('losses', 0)}L ({ab.get('percent', 0):.1f}%)"
        )

    def _run_gate_only(self) -> None:
        net = Path(self.validate_net_var.get())
        if not net.exists():
            self.messagebox.showerror(APP_TITLE, "Choose a network first")
            return
        self._validation_worker(net, games=0, nodes=0, gate_only=True)

    def _start_validation(self, auto: bool) -> None:
        net = Path(self.validate_net_var.get())
        if not net.exists():
            if auto:
                self.status_var.set("no network to validate")
                return
            self.messagebox.showerror(APP_TITLE, "Choose a network first")
            return
        try:
            games = core.parse_int(self.games_ab_var.get(), "A/B games", 2, 2000)
            nodes = core.parse_int(self.nodes_ab_var.get(), "A/B nodes", 1)
        except ValueError as error:
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        self._validation_worker(net, games=games, nodes=nodes)

    def _validation_worker(
        self,
        net: Path,
        games: int,
        nodes: int,
        gate_only: bool = False,
        adopt: bool | None = None,
    ) -> None:
        active = self.active_run
        token = core.CancelToken()
        self.validation_cancel = token
        started = time.monotonic()

        def worker() -> None:
            run = active if active is not None and net == active.net_path else None
            result = core.validate_net(
                net,
                run=run,
                games=games,
                nodes=nodes,
                report_path=net.parent / "ab-match.json",
                gate_only=gate_only,
                cancel=token,
                on_line=lambda line: self.queue.put(("log", line + "\n")),
            )
            result["elapsed_seconds"] = time.monotonic() - started
            wants_adopt = self.auto_adopt_var.get() if adopt is None else adopt
            if wants_adopt and not gate_only:
                result["adoption"] = self._adoption_worker(net, result)
            self.queue.put(("validation", {"net": str(net), "result": result}))
            self.validation_cancel = None

        self.validation_text.delete("1.0", "end")
        self.validation_text.insert("end", f"Validating {net}\n")
        self.cancel_validation_button.configure(state="normal")
        self.status_var.set("validating")
        threading.Thread(target=worker, daemon=True).start()

    def _cancel_validation(self) -> None:
        """Ask the running gate/match process tree to stop."""
        if self.validation_cancel is not None:
            self.validation_cancel.cancel()
            self.status_var.set("cancelling validation")
            self._append_log("\n[studio] validation cancel requested\n")

    def _adoption_worker(self, net: Path, result: dict) -> dict:
        """Compare against the installed network and adopt per policy (worker thread)."""
        engine_dir = Path(self.engine_dir_var.get())
        installed = engine_dir / "koi.nnue"
        try:
            games = core.parse_int(self.games_ab_var.get(), "A/B games", 2, 2000)
            nodes = core.parse_int(self.nodes_ab_var.get(), "A/B nodes", 1)
        except ValueError:
            games, nodes = 20, 20_000
        comparison: dict | None = None
        if installed.exists() and installed.resolve() != net.resolve():
            try:
                comparison = core.run_net_match(net, installed, games=games, nodes=nodes)
            except Exception as error:  # noqa: BLE001 - surfaced as evidence
                comparison = {"error": str(error)}
        decision = core.adoption_decision(
            result.get("gate"), comparison, installed if installed.exists() else None
        )
        adoption: dict = {"decision": decision, "net_match": comparison}
        if decision["decision"] == "adopt":
            try:
                ab = result.get("ab_match") or {}
                adoption["install"] = core.adopt_net(
                    net,
                    engine_dir,
                    evidence={
                        "source_run": self.active_run.directory.name if self.active_run else None,
                        "gate": result.get("gate"),
                        "ab_verdict": ab.get("verdict"),
                        "net_match_verdict": (comparison or {}).get("verdict"),
                    },
                )
                adoption["running_engines"] = core.running_engines()
            except Exception as error:  # noqa: BLE001 - surfaced as evidence
                adoption["error"] = str(error)
        return adoption

    def _net_match_report(self, comparison: dict) -> str:
        if comparison.get("error"):
            return f"net match: unavailable ({comparison['error']})"
        return (
            f"net match: {comparison.get('games', 0)} games "
            f"+{comparison.get('wins', 0)} ={comparison.get('draws', 0)} "
            f"-{comparison.get('losses', 0)} ({comparison.get('percent', 0)}%) "
            f"{comparison.get('verdict', 'inconclusive')}"
        )

    def _adoption_report(self, adoption: dict) -> str:
        decision = adoption.get("decision") or {}
        lines = [f"auto-adopt: {decision.get('decision')} - {decision.get('reason')}"]
        comparison = adoption.get("net_match")
        if comparison:
            lines.append(self._net_match_report(comparison))
        install = adoption.get("install")
        if install:
            lines.append(f"installed {install['installed']}")
            if install.get("backup"):
                lines.append(f"previous network backed up to {install['backup']}")
            engines = adoption.get("running_engines") or []
            if engines:
                pids = ", ".join(str(pid) for pid in engines)
                lines.append(
                    f"koi-engine is running (pid {pids}); restart it or send "
                    f"'setoption name EvalFile {install['installed']}' to load the new network"
                )
        if adoption.get("error"):
            lines.append(f"adoption error: {adoption['error']}")
        return "\n".join(lines)

    def _finish_adoption(self, net: str, gate: dict, adoption: dict) -> None:
        self.validation_text.see("end")
        decision = (adoption.get("decision") or {}).get("decision")
        if decision == "adopt" and adoption.get("install"):
            self.install_button.configure(state="normal")
            self.status_var.set(f"auto-adopted {net}")
            self._save_settings()
            self._refresh_runs()
        elif decision == "adopt":
            self.install_button.configure(state="disabled")
            self.status_var.set("adoption failed")
        else:
            self.install_button.configure(
                state="normal" if core.gate_allows_install(gate) else "disabled"
            )
            self.status_var.set("auto-adopt skipped - manual install stays available")

    def _revert_adoption(self) -> None:
        """Restore the backup recorded by the most recent auto-adoption."""
        try:
            result = core.revert_adoption()
        except Exception as error:  # noqa: BLE001 - reported in the dialog
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        if result.get("error"):
            self.messagebox.showinfo(APP_TITLE, result["error"])
            return
        self.messagebox.showinfo(
            APP_TITLE,
            f"Restored {result['restored_from']}\nover {result['reverted']}",
        )
        self.status_var.set("reverted the last adoption")

    def _save_settings(self) -> None:
        settings = dict(self.settings)
        settings["engine_directory"] = self.engine_dir_var.get()
        settings["auto_adopt"] = bool(self.auto_adopt_var.get())
        settings["theme"] = self.theme_var.get()
        try:
            settings["geometry"] = self.root.geometry()
        except Exception:  # noqa: BLE001 - geometry is best-effort
            pass
        try:
            settings["ab_games"] = core.parse_int(self.games_ab_var.get(), "A/B games", 2, 2000)
            settings["ab_nodes"] = core.parse_int(self.nodes_ab_var.get(), "A/B nodes", 1)
        except ValueError:
            pass
        self.settings = settings
        core.write_settings(settings)

    def _on_close(self) -> None:
        self._save_settings()
        self.root.destroy()

    def _install_net(self) -> None:
        net = Path(self.validate_net_var.get())
        if not net.exists():
            self.messagebox.showerror(APP_TITLE, "Choose a network first")
            return
        engine_dir = Path(self.engine_dir_var.get())
        self.install_button.configure(state="disabled")
        self.status_var.set("installing network")

        def worker() -> None:
            try:
                info = core.install_net(net, engine_dir)
            except Exception as error:  # noqa: BLE001 - reported in the dialog
                self.queue.put(("install_error", str(error)))
                return
            self.queue.put(("installed", info))

        threading.Thread(target=worker, daemon=True).start()

    # -- data generation ----------------------------------------------------

    def _data_run(self, argv: list[str]) -> None:
        run = core.create_run("data", {"argv": argv}, "datagen")
        core.build_command_file(run, argv)
        try:
            core.launch_run(run)
        except Exception as error:  # noqa: BLE001 - reported in the dialog
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        self.messagebox.showinfo(
            APP_TITLE, f"Started {run.directory.name}\nLog: {run.log_path}"
        )

    def _start_generate(self) -> None:
        argv = [
            core.python_executable(),
            str(core.REPO_ROOT / "tools" / "measurement" / "gen_training_data.py"),
            "games",
            "--games",
            self.games_var.get(),
            "--workers",
            self.workers_var.get(),
            "--game-depth",
            self.game_depth_var.get(),
            "--positions",
            self.positions_var.get(),
        ]
        self._data_run(argv)

    def _start_label(self) -> None:
        argv = [
            core.python_executable(),
            str(core.REPO_ROOT / "tools" / "measurement" / "gen_training_data.py"),
            "label",
            "--workers",
            self.workers_var.get(),
            "--label-depth",
            self.label_depth_var.get(),
            "--limit",
            self.label_limit_var.get(),
            "--positions",
            self.positions_var.get(),
            "--output",
            self.labels_var.get(),
        ]
        if self.resume_var.get():
            argv.append("--resume")
        self._data_run(argv)

    # -- runs list ----------------------------------------------------------

    def _refresh_runs(self) -> None:
        if self.runs_refreshing:
            return
        self.runs_refreshing = True
        selected = self._selected_name()
        needle = self.runs_filter_var.get()
        key, descending = self.runs_sort

        def worker() -> None:
            rows: list[tuple] = []
            try:
                runs = core.filter_runs(core.list_runs(), needle)
                runs = core.sort_runs(runs, key, descending)
                for run in runs:
                    state = core.refresh_state(run)
                    progress = state.get("progress", {})
                    backend = self._run_backend(run)
                    net = backend.net_path(run.directory, run.config) if backend else run.net_path
                    duration = core.run_duration_seconds(state)
                    rows.append(
                        (
                            run.directory.name,
                            state.get("kind", "?"),
                            state.get("status", "unknown"),
                            progress.get("val_mae_cp", ""),
                            core.format_duration(duration) if duration is not None else "",
                            str(net) if net.exists() else "",
                        )
                    )
            finally:
                self.queue.put(("runs", {"rows": rows, "selected": selected}))

        threading.Thread(target=worker, daemon=True).start()

    def _apply_runs(self, payload: dict) -> None:
        rows = payload.get("rows", [])
        selected = payload.get("selected")
        self.runs_tree.delete(*self.runs_tree.get_children())
        for row in rows:
            self.runs_tree.insert("", "end", iid=row[0], values=row)
        if selected and self.runs_tree.exists(selected):
            self.runs_tree.selection_set(selected)
        self._show_run_detail()
        self.status_var.set(f"{len(rows)} runs")
        self.runs_refreshing = False

    def _sort_runs(self, key: str) -> None:
        current_key, descending = self.runs_sort
        self.runs_sort = (key, not descending if key == current_key else False)
        self._refresh_runs()

    def _show_run_detail(self) -> None:
        run = self._selected_run()
        self.runs_detail.configure(state="normal")
        self.runs_detail.delete("1.0", "end")
        if run:
            self.runs_detail.insert("1.0", "\n".join(core.run_detail_lines(run)))
        self.runs_detail.configure(state="disabled")

    def _selected_name(self) -> str | None:
        selection = self.runs_tree.selection()
        return selection[0] if selection else None

    def _selected_run(self) -> core.Run | None:
        name = self._selected_name()
        if not name:
            return None
        directory = core.RUNS_DIR / name
        return core.load_run(directory) if directory.exists() else None

    def _attach_selected(self) -> None:
        run = self._selected_run()
        if run:
            self._attach(run)

    def _use_selected_net(self) -> None:
        run = self._selected_run()
        if not run:
            return
        backend = self._run_backend(run)
        net = backend.net_path(run.directory, run.config) if backend else run.net_path
        if net.exists():
            self.validate_net_var.set(str(net))
        else:
            self.messagebox.showinfo(APP_TITLE, "That run has no network yet")

    def _open_selected_run(self) -> None:
        run = self._selected_run()
        if run:
            self._open_path(run.directory)

    def _stop_selected(self) -> None:
        run = self._selected_run()
        if run:
            core.stop_run(run)
            self._refresh_runs()

    def _adopt_best_run(self) -> None:
        """Gate, compare, and adopt the completed run with the lowest val MAE."""
        candidates = [
            run
            for run in core.list_runs()
            if run.state.get("status") == "completed" and run.net_path.exists()
        ]
        if not candidates:
            self.messagebox.showinfo(APP_TITLE, "No completed run has a network yet")
            return
        best = core.sort_runs(candidates, "val_mae")[0]
        self.validate_net_var.set(str(best.net_path))
        try:
            games = core.parse_int(self.games_ab_var.get(), "A/B games", 2, 2000)
            nodes = core.parse_int(self.nodes_ab_var.get(), "A/B nodes", 1)
        except ValueError as error:
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        self.status_var.set(f"adopting best run {best.directory.name}")
        self._validation_worker(best.net_path, games=games, nodes=nodes, adopt=True)

    # -- event pump ---------------------------------------------------------

    def _append_log(self, text: str) -> None:
        at_bottom = self.log_text.yview()[1] >= 0.999
        needle = self.log_filter_var.get()
        errors_only = self.log_errors_var.get()
        for line in text.splitlines(keepends=True):
            self.log_lines.append(line)
            if core.log_line_matches(line, needle, errors_only):
                self.log_text.insert("end", line)
        if len(self.log_lines) > 6000:
            self.log_lines = self.log_lines[-5000:]
            self._render_log()
            return
        if at_bottom:
            self.log_text.see("end")

    def _render_log(self) -> None:
        self.log_text.delete("1.0", "end")
        needle = self.log_filter_var.get()
        errors_only = self.log_errors_var.get()
        for line in self.log_lines:
            if core.log_line_matches(line, needle, errors_only):
                self.log_text.insert("end", line)
        self.log_text.see("end")

    def _draw_chart(self) -> None:
        self.chart.delete("all")
        self._chart_hover = None
        self._chart_geometry = None
        series = core.chart_series(self.chart_progress)
        loss_bounds = core.chart_bounds(series, "loss")
        mae_bounds = core.chart_bounds(series, "mae")
        primary = loss_bounds or mae_bounds
        if primary is None:
            self.chart.create_text(12, 12, anchor="nw", text="no epochs yet", fill="#666666")
            return
        width = max(int(self.chart.winfo_width()), 100)
        height = max(int(self.chart.winfo_height()), 60)
        margin_left = 46
        margin_right = 52 if (loss_bounds and mae_bounds) else 12
        margin_top = 24
        margin_bottom = 22
        low, high = primary
        span = max(high - low, 1e-9)
        count = max((len(item["values"]) for item in series), default=0)
        plot_width = max(width - margin_left - margin_right, 10)
        plot_height = max(height - margin_top - margin_bottom, 10)

        self.chart.create_line(margin_left, margin_top, margin_left, height - margin_bottom, fill="#999999")
        self.chart.create_line(
            margin_left, height - margin_bottom, width - margin_right, height - margin_bottom, fill="#999999"
        )
        for step in range(3):
            y = height - margin_bottom - plot_height * step / 2
            self.chart.create_line(margin_left, y, width - margin_right, y, fill="#eeeeee")
            self.chart.create_text(
                margin_left - 4, y, anchor="e", text=f"{low + span * step / 2:.0f}", fill="#666666"
            )
        if loss_bounds and mae_bounds:
            mae_span = max(mae_bounds[1] - mae_bounds[0], 1e-9)
            for step in range(3):
                y = height - margin_bottom - plot_height * step / 2
                self.chart.create_text(
                    width - margin_right + 4,
                    y,
                    anchor="w",
                    text=f"{mae_bounds[0] + mae_span * step / 2:.0f}",
                    fill="#1f6fb2",
                )
        if count > 1:
            tick_step = max(1, count // 6)
            for index in range(0, count, tick_step):
                x = margin_left + plot_width * (index / max(count - 1, 1))
                self.chart.create_text(
                    x, height - margin_bottom + 4, anchor="n", text=str(index + 1), fill="#666666"
                )

        legend_x = margin_left
        for item in series:
            bounds = loss_bounds if item["axis"] == "loss" else mae_bounds
            if bounds is None:
                continue
            item_low, item_high = bounds
            item_span = max(item_high - item_low, 1e-9)
            points = []
            for index, value in enumerate(item["values"]):
                x = margin_left + plot_width * (index / max(count - 1, 1))
                y = height - margin_bottom - plot_height * ((value - item_low) / item_span)
                points.append((x, y))
            if len(points) > 1:
                self.chart.create_line(
                    *[coordinate for point in points for coordinate in point],
                    fill=item["color"],
                    width=2,
                )
            for x, y in points:
                self.chart.create_oval(x - 2, y - 2, x + 2, y + 2, fill=item["color"], outline="")
            self.chart.create_rectangle(legend_x, 6, legend_x + 8, 14, fill=item["color"], outline="")
            self.chart.create_text(legend_x + 12, 10, anchor="w", text=item["name"], fill="#444444")
            legend_x += 36 + 7 * len(item["name"])
        self._chart_geometry = (margin_left, plot_width, count, height, margin_bottom, plot_height, series)

    def _on_chart_motion(self, event) -> None:
        """Show the nearest epoch's values while hovering the chart."""
        if self._chart_geometry is None:
            return
        margin_left, plot_width, count, height, margin_bottom, plot_height, series = self._chart_geometry
        index = core.chart_hover_index(event.x, margin_left, plot_width, count)
        if index == self._chart_hover:
            return
        self.chart.delete("hover")
        self._chart_hover = index
        if index is None:
            return
        colors = core.palette(self.theme_var.get())
        lines = []
        for item in series:
            values = item["values"]
            if index < len(values):
                lines.append(f"{item['name']}: {values[index]:.4f}")
        x = margin_left + plot_width * (index / max(count - 1, 1))
        self.chart.create_line(x, 24, x, height - margin_bottom, fill=colors["muted"], tags="hover")
        self.chart.create_text(
            x + 6,
            26,
            anchor="nw",
            text=f"epoch {index + 1}\n" + "\n".join(lines),
            fill=colors["text"],
            tags="hover",
        )

    def _export_chart(self) -> None:
        path = self.filedialog.asksaveasfilename(
            title="Export chart",
            defaultextension=".ps",
            filetypes=[("PostScript", "*.ps"), ("PNG (requires Pillow)", "*.png")],
        )
        if not path:
            return
        target = Path(path)
        try:
            self.chart.postscript(file=str(target), colormode="color")
        except Exception as error:  # noqa: BLE001 - reported in the dialog
            self.messagebox.showerror(APP_TITLE, f"chart export failed: {error}")
            return
        if target.suffix.lower() == ".png":
            try:
                from PIL import Image

                with Image.open(str(target)) as image:
                    image.save(str(target))
            except Exception:  # noqa: BLE001 - Pillow/Ghostscript are optional
                fallback = target.with_suffix(".ps")
                target.replace(fallback)
                self.messagebox.showinfo(APP_TITLE, f"Pillow is unavailable; wrote {fallback}")
                self.status_var.set(f"exported chart to {fallback}")
                return
        self.status_var.set(f"exported chart to {target}")

    def _pump(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "log":
                    self._append_log(payload)
                elif kind == "counts":
                    positions, labels = payload
                    self.data_counts_var.set(
                        f"positions: {positions:,} lines   labels: {labels:,} rows"
                    )
                    self.status_var.set("Counted corpus rows")
                elif kind == "runs":
                    self._apply_runs(payload)
                elif kind == "installed":
                    message = f"Installed {payload['installed']}"
                    if payload.get("backup"):
                        message += f"\nPrevious network backed up to {payload['backup']}"
                    self.messagebox.showinfo(APP_TITLE, message)
                    self.install_button.configure(state="normal")
                    self.status_var.set(f"installed {payload['installed']}")
                elif kind == "install_error":
                    self.messagebox.showerror(APP_TITLE, str(payload))
                    self.install_button.configure(state="normal")
                elif kind == "progress":
                    progress = payload
                    epochs = max(1, int(progress.get("epochs", 1)))
                    self.progress.configure(maximum=epochs)
                    self.progress["value"] = int(progress.get("epoch", 0))
                    self.chart_progress = progress
                    self._draw_chart()
                    self.progress_var.set(core.progress_summary(progress))
                elif kind == "status":
                    self.status_var.set(f"training: {payload}")
                elif kind == "finished":
                    run = payload
                    state = core.refresh_state(run)
                    self.stop_button.configure(state="disabled")
                    self._append_log(f"\n[studio] run finished with status {state.get('status')}\n")
                    if state.get("status") == "failed":
                        failures = core.failure_lines(run)
                        if failures:
                            self._append_log("[studio] train.err tail:\n")
                            for line in failures:
                                self._append_log(f"  {line}\n")
                        prompt = f"Run {run.directory.name} failed"
                        if state.get("exit_code") is not None:
                            prompt += f" (exit code {state['exit_code']})"
                        if self.messagebox.askyesno(APP_TITLE, prompt + ".\n\nOpen the run folder?"):
                            subprocess.Popen(["explorer", str(run.directory)])
                    backend = self._run_backend(run)
                    net = backend.net_path(run.directory, run.config) if backend else run.net_path
                    if net.exists():
                        self.validate_net_var.set(str(net))
                    if (
                        state.get("status") == "completed"
                        and net.exists()
                        and self.auto_validate_var.get()
                    ):
                        self._append_log("[studio] starting validation\n")
                        self._start_validation(auto=True)
                    self._refresh_runs()
                elif kind == "validation":
                    net = payload["net"]
                    result = payload["result"]
                    gate = result.get("gate", {})
                    ab = result.get("ab_match")
                    adoption = result.get("adoption")
                    self.cancel_validation_button.configure(state="disabled")
                    if result.get("cancelled"):
                        self.validation_text.insert("end", "validation cancelled\n")
                    elapsed = result.get("elapsed_seconds")
                    if elapsed:
                        self.validation_text.insert(
                            "end", f"elapsed {core.format_duration(elapsed)}\n"
                        )
                    self.validation_text.insert("end", self._gate_report(gate) + "\n")
                    if ab is not None:
                        self.validation_text.insert("end", self._ab_report(ab) + "\n")
                    if adoption is not None:
                        self.validation_text.insert(
                            "end", self._adoption_report(adoption) + "\n"
                        )
                        self._finish_adoption(net, gate, adoption)
                    elif core.gate_allows_install(gate):
                        self.validation_text.see("end")
                        self.status_var.set("validation complete")
                        self.install_button.configure(state="normal")
                        prompt = f"{self._gate_report(gate)}\n"
                        if ab is not None:
                            prompt += f"{self._ab_report(ab)}\n"
                        if self.messagebox.askyesno(
                            APP_TITLE,
                            f"{prompt}\nInstall {net} as the engine network?",
                        ):
                            self._install_net()
                    else:
                        self.install_button.configure(state="disabled")
                        self.validation_text.insert(
                            "end",
                            "install stays disabled until a 64-position gate passes\n",
                        )
                        self.validation_text.see("end")
                        self.status_var.set("validation incomplete - install disabled")
        except queue.Empty:
            pass
        self.root.after(120, self._pump)

    def run(self) -> int:
        self._refresh_runs()
        self._schedule_auto_refresh()
        self.root.mainloop()
        return 0

    def _schedule_auto_refresh(self) -> None:
        self._refresh_runs()
        self.root.after(5000, self._schedule_auto_refresh)


def gui_selftest() -> int:
    """Construct the GUI, apply both themes, pump one frame, and exit."""
    app = StudioApp()
    app._apply_theme("dark")
    app._apply_theme("light")
    app.root.after(400, app.root.destroy)
    app.root.mainloop()
    print("PASS gui construction")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--list-backends", action="store_true", help="show trainer backends and exit")
    parser.add_argument("--dry-run", action="store_true", help="print the trainer command and exit")
    parser.add_argument("--run", metavar="PRESET", help="train headlessly with a preset (quick/standard/thorough)")
    parser.add_argument("--detach", action="store_true", help="with --run, launch detached instead of waiting")
    parser.add_argument("--selftest", action="store_true", help="train a tiny network and check the engine can load it")
    parser.add_argument("--gui-selftest", action="store_true", help="construct the GUI and exit")
    parser.add_argument("--preset", choices=list(core.presets()), help="preset used by --dry-run/--run/--selftest")
    parser.add_argument("--backend", default=core.default_config()["backend"], help="trainer backend name")
    parser.add_argument("--corpus", help="training corpus (FEN;cp;bestmove rows)")
    parser.add_argument("--epochs", type=int, help="override the epoch count")
    parser.add_argument("--batch-size", type=int, help="override the batch size")
    parser.add_argument("--learning-rate", type=float, help="override the learning rate")
    parser.add_argument("--threads", type=int, help="override the torch thread count")
    parser.add_argument("--rows", type=int, help="cap the number of training rows (0 = all)")
    parser.add_argument("--net-name", help="network file name inside the run directory")
    parser.add_argument("--no-gate", action="store_true", help="with --selftest, skip the engine load check")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.list_backends:
        return print_backends()
    if args.dry_run:
        return dry_run(args)
    if args.selftest:
        return selftest(args)
    if args.run:
        return run_headless(args)
    if args.gui_selftest:
        return gui_selftest()
    return StudioApp().run()


if __name__ == "__main__":
    raise SystemExit(main())
