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
    history: list[float] = []
    with open(run.log_path, "w", encoding="utf-8") as log:
        for line in process.stdout:
            log.write(line)
            log.flush()
            if echo:
                print(line, end="", flush=True)
            event = core.parse_progress(line)
            if event and event.get("kind") == "epoch":
                history.append(event["val_mae_cp"])
                run.write_state(
                    progress={
                        "epoch": event["epoch"],
                        "epochs": event["epochs"],
                        "val_mae_cp": event["val_mae_cp"],
                        "history": history,
                    }
                )
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
    run = core.create_run("selftest", config, backend.name)
    command = backend.build_command(run.directory, config)
    core.build_command_file(run, command)
    print(f"[selftest] run directory {run.directory}")
    print(f"[selftest] command {subprocess.list2cmdline(command)}")
    code = stream_command(command, run)
    if code != 0:
        print(f"[selftest] trainer exited with {code}", file=sys.stderr)
        return code
    net = backend.net_path(run.directory)
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
        self.root = tk.Tk()
        self.root.title(f"{APP_TITLE} {core.STUDIO_VERSION}")
        self.root.geometry("1000x780")
        self.root.minsize(820, 620)
        self.queue: queue.Queue = queue.Queue()
        self.active_run: core.Run | None = None
        self.log_offset = 0
        self.tail_stop = threading.Event()
        self.busy = False

        self.backends = available_backends()
        self.backend_names = [backend.name for backend in self.backends]
        self.preset_names = list(core.presets())
        self._build_style()
        self._build_layout()
        self._pump()

    # -- construction -------------------------------------------------------

    def _build_style(self) -> None:
        style = self.ttk.Style(self.root)
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Heading.TLabel", font=("Segoe UI", 10, "bold"))
        style.configure("Status.TLabel", foreground="#333333")

    def _build_layout(self) -> None:
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

    def _build_data_tab(self, notebook) -> None:
        tk = self.tk
        frame = tk.ttk.Frame(notebook, padding=12)
        notebook.add(frame, text="Data")

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
                "this machine at depth 10."
            ),
        ).grid(row=5, column=0, columnspan=4, sticky="w")
        frame.columnconfigure(1, weight=1)

    def _build_train_tab(self, notebook) -> None:
        tk = self.tk
        frame = tk.ttk.Frame(notebook, padding=12)
        notebook.add(frame, text="Train")

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
        tk.ttk.Label(buttons, text="A/B games").pack(side="left")
        tk.ttk.Entry(buttons, textvariable=self.games_ab_var, width=5).pack(side="left", padx=(4, 10))
        tk.ttk.Label(buttons, text="A/B nodes").pack(side="left")
        tk.ttk.Entry(buttons, textvariable=self.nodes_ab_var, width=8).pack(side="left", padx=4)

        self.progress = tk.ttk.Progressbar(frame, mode="determinate", maximum=10)
        self.progress.grid(row=5, column=0, columnspan=6, sticky="we", pady=(4, 2))
        self.progress_var = tk.StringVar(value="idle")
        tk.ttk.Label(frame, textvariable=self.progress_var).grid(row=6, column=0, columnspan=6, sticky="w")

        tk.ttk.Label(frame, text="Validation MAE per epoch (cp)", style="Heading.TLabel").grid(
            row=7, column=0, columnspan=6, sticky="w", pady=(8, 0)
        )
        self.chart = tk.Canvas(frame, height=110, background="#fdfdfd", highlightthickness=1, highlightbackground="#cccccc")
        self.chart.grid(row=8, column=0, columnspan=6, sticky="we", pady=(2, 8))
        self.chart.bind("<Configure>", lambda _event: self._draw_chart())

        tk.ttk.Label(frame, text="Log", style="Heading.TLabel").grid(row=9, column=0, sticky="w")
        self.log_text = tk.Text(frame, height=14, wrap="none", background="#111111", foreground="#d8d8d8")
        self.log_text.grid(row=10, column=0, columnspan=6, sticky="nsew")
        scrollbar = tk.ttk.Scrollbar(frame, command=self.log_text.yview)
        scrollbar.grid(row=10, column=6, sticky="ns")
        self.log_text.configure(yscrollcommand=scrollbar.set)
        frame.rowconfigure(10, weight=1)
        frame.columnconfigure(5, weight=1)
        self.chart_history: list[float] = []
        self._backend_changed()

    def _build_validate_tab(self, notebook) -> None:
        tk = self.tk
        frame = tk.ttk.Frame(notebook, padding=12)
        notebook.add(frame, text="Validate and install")

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

        self.validation_text = tk.Text(frame, height=16, wrap="word")
        self.validation_text.grid(row=3, column=0, columnspan=5, sticky="nsew")
        frame.rowconfigure(3, weight=1)

        install = tk.ttk.LabelFrame(frame, text="Install", padding=10)
        install.grid(row=4, column=0, columnspan=5, sticky="we", pady=10)
        self.engine_dir_var = tk.StringVar(value=str(core.DEFAULT_BUILD_DIR))
        tk.ttk.Label(install, text="Engine directory").grid(row=0, column=0, sticky="w")
        tk.ttk.Entry(install, textvariable=self.engine_dir_var, width=60).grid(
            row=0, column=1, columnspan=3, sticky="we", padx=6
        )
        self.install_button = tk.ttk.Button(
            install, text="Install as koi.nnue", command=self._install_net, state="disabled"
        )
        self.install_button.grid(row=1, column=1, sticky="w", pady=(8, 0))
        tk.ttk.Label(
            install,
            text="Installing copies the network beside the engine (backing up any previous koi.nnue).",
            foreground="#555555",
        ).grid(row=2, column=0, columnspan=4, sticky="w", pady=(6, 0))
        frame.columnconfigure(0, weight=1)

    def _build_runs_tab(self, notebook) -> None:
        tk = self.tk
        frame = tk.ttk.Frame(notebook, padding=12)
        notebook.add(frame, text="Runs")

        columns = ("run", "kind", "status", "val_mae", "net")
        self.runs_tree = tk.ttk.Treeview(frame, columns=columns, show="headings", height=16)
        for column, heading, width in [
            ("run", "Run", 260),
            ("kind", "Kind", 80),
            ("status", "Status", 90),
            ("val_mae", "Val MAE (cp)", 100),
            ("net", "Network", 240),
        ]:
            self.runs_tree.heading(column, text=heading)
            self.runs_tree.column(column, width=width, anchor="w")
        self.runs_tree.grid(row=0, column=0, columnspan=6, sticky="nsew")
        frame.rowconfigure(0, weight=1)

        tk.ttk.Button(frame, text="Refresh", command=self._refresh_runs).grid(row=1, column=0, sticky="w", pady=8)
        tk.ttk.Button(frame, text="Attach to selected", command=self._attach_selected).grid(row=1, column=1, padx=4)
        tk.ttk.Button(frame, text="Use network", command=self._use_selected_net).grid(row=1, column=2, padx=4)
        tk.ttk.Button(frame, text="Open folder", command=self._open_selected_run).grid(row=1, column=3, padx=4)
        tk.ttk.Button(frame, text="Stop selected", command=self._stop_selected).grid(row=1, column=4, padx=4)

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
        positions = core.count_rows(self.positions_var.get())
        labels = core.count_rows(self.labels_var.get())
        self.data_counts_var.set(f"positions: {positions:,} lines   labels: {labels:,} rows")
        self.status_var.set("Counted corpus rows")

    def _open_path(self, path: Path) -> None:
        if path.exists():
            self.messagebox.showinfo(APP_TITLE, f"Opening {path}")
            subprocess.Popen(["explorer", str(path)])

    def _collect_config(self) -> dict:
        config = core.default_config()
        config["backend"] = self.backend_var.get()
        config["corpus"] = str(Path(self.labels_var.get()))
        config["epochs"] = int(self.epochs_var.get())
        config["batch_size"] = int(self.batch_var.get())
        config["learning_rate"] = float(self.lr_var.get())
        config["threads"] = int(self.threads_var.get())
        config["rows"] = int(self.rows_var.get())
        config["val_fraction"] = float(self.val_fraction_var.get())
        config["net_name"] = self.net_name_var.get()
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
        self.chart_history = []
        self.log_text.delete("1.0", "end")
        self.progress.configure(maximum=max(1, int(run.config.get("epochs", 1))))
        self.progress["value"] = 0
        self.stop_button.configure(state="normal")
        self.open_run_button.configure(state="normal")
        self.tail_stop = threading.Event()
        thread = threading.Thread(target=self._tail_loop, args=(run, self.tail_stop), daemon=True)
        thread.start()
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
            net = backend.net_path(run.directory)
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
        self._validation_worker(net, games=0, nodes=0)

    def _start_validation(self, auto: bool) -> None:
        net = Path(self.validate_net_var.get())
        if not net.exists():
            if auto:
                self.status_var.set("no network to validate")
                return
            self.messagebox.showerror(APP_TITLE, "Choose a network first")
            return
        games = int(self.games_ab_var.get())
        nodes = int(self.nodes_ab_var.get())
        self._validation_worker(net, games=games, nodes=nodes)

    def _validation_worker(self, net: Path, games: int, nodes: int) -> None:
        active = self.active_run

        def worker() -> None:
            run = active if active is not None and net == active.net_path else None
            result = core.validate_net(
                net, run=run, games=games, nodes=nodes, report_path=net.parent / "ab-match.json"
            )
            self.queue.put(("validation", {"net": str(net), "result": result}))

        self.validation_text.delete("1.0", "end")
        self.validation_text.insert("end", f"Validating {net}\n")
        self.status_var.set("validating")
        threading.Thread(target=worker, daemon=True).start()

    def _install_net(self) -> None:
        net = Path(self.validate_net_var.get())
        if not net.exists():
            self.messagebox.showerror(APP_TITLE, "Choose a network first")
            return
        try:
            info = core.install_net(net, Path(self.engine_dir_var.get()))
        except Exception as error:  # noqa: BLE001 - reported in the dialog
            self.messagebox.showerror(APP_TITLE, str(error))
            return
        message = f"Installed {info['installed']}"
        if info.get("backup"):
            message += f"\nPrevious network backed up to {info['backup']}"
        self.messagebox.showinfo(APP_TITLE, message)
        self.status_var.set(f"installed {info['installed']}")

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
        self.runs_tree.delete(*self.runs_tree.get_children())
        for run in core.list_runs():
            state = core.refresh_state(run)
            progress = state.get("progress", {})
            backend = self._run_backend(run)
            net = backend.net_path(run.directory) if backend else run.directory / "net.nnue"
            self.runs_tree.insert(
                "",
                "end",
                values=(
                    run.directory.name,
                    state.get("kind", "?"),
                    state.get("status", "unknown"),
                    progress.get("val_mae_cp", ""),
                    str(net) if net.exists() else "",
                ),
            )
        self.status_var.set(f"{len(self.runs_tree.get_children())} runs")

    def _selected_run(self) -> core.Run | None:
        selection = self.runs_tree.selection()
        if not selection:
            return None
        name = self.runs_tree.item(selection[0], "values")[0]
        return core.load_run(core.RUNS_DIR / name)

    def _attach_selected(self) -> None:
        run = self._selected_run()
        if run:
            self._attach(run)

    def _use_selected_net(self) -> None:
        run = self._selected_run()
        if not run:
            return
        backend = self._run_backend(run)
        net = backend.net_path(run.directory) if backend else run.directory / "net.nnue"
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

    # -- event pump ---------------------------------------------------------

    def _append_log(self, text: str) -> None:
        self.log_text.insert("end", text)
        lines = int(self.log_text.index("end-1c").split(".")[0])
        if lines > 6000:
            self.log_text.delete("1.0", f"{lines - 5000}.0")
        self.log_text.see("end")

    def _draw_chart(self) -> None:
        self.chart.delete("all")
        if not self.chart_history:
            return
        width = max(int(self.chart.winfo_width()), 100)
        height = max(int(self.chart.winfo_height()), 60)
        margin = 12
        low = min(self.chart_history)
        high = max(self.chart_history)
        span = max(high - low, 1.0)
        count = len(self.chart_history)
        points = []
        for index, value in enumerate(self.chart_history):
            x = margin + (width - 2 * margin) * (index / max(count - 1, 1))
            y = height - margin - (height - 2 * margin) * ((value - low) / span)
            points.append((x, y))
        if len(points) > 1:
            self.chart.create_line(*[coordinate for point in points for coordinate in point], fill="#1f6fb2", width=2)
        for x, y in points:
            self.chart.create_oval(x - 2, y - 2, x + 2, y + 2, fill="#1f6fb2", outline="")
        self.chart.create_text(margin, margin, anchor="nw", text=f"min {low:.0f}", fill="#666666")
        self.chart.create_text(margin, height - margin, anchor="sw", text=f"max {high:.0f}", fill="#666666")

    def _pump(self) -> None:
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "log":
                    self._append_log(payload)
                elif kind == "progress":
                    progress = payload
                    epochs = max(1, int(progress.get("epochs", 1)))
                    self.progress.configure(maximum=epochs)
                    self.progress["value"] = int(progress.get("epoch", 0))
                    self.chart_history = [float(value) for value in progress.get("history", [])]
                    self._draw_chart()
                    if progress.get("val_mae_cp") is not None:
                        self.progress_var.set(
                            f"epoch {progress.get('epoch', 0)}/{epochs} - val MAE "
                            f"{progress['val_mae_cp']:.1f} cp"
                        )
                elif kind == "status":
                    self.status_var.set(f"training: {payload}")
                elif kind == "finished":
                    run = payload
                    state = core.refresh_state(run)
                    self.stop_button.configure(state="disabled")
                    self._append_log(f"\n[studio] run finished with status {state.get('status')}\n")
                    backend = self._run_backend(run)
                    net = backend.net_path(run.directory) if backend else run.directory / "net.nnue"
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
                    ab = result.get("ab_match", {})
                    self.validation_text.insert("end", self._gate_report(gate) + "\n")
                    self.validation_text.insert("end", self._ab_report(ab) + "\n")
                    self.validation_text.see("end")
                    self.status_var.set("validation complete")
                    if not gate.get("rejected"):
                        self.install_button.configure(state="normal")
                        if self.messagebox.askyesno(
                            APP_TITLE,
                            f"{self._gate_report(gate)}\n{self._ab_report(ab)}\n\nInstall {net} as the engine network?",
                        ):
                            self._install_net()
        except queue.Empty:
            pass
        self.root.after(120, self._pump)

    def run(self) -> int:
        self._refresh_runs()
        self.root.mainloop()
        return 0


def gui_selftest() -> int:
    """Construct the GUI, pump one frame, and exit (widget smoke test)."""
    app = StudioApp()
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
    parser.add_argument("--backend", default="torch", help="trainer backend name")
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
