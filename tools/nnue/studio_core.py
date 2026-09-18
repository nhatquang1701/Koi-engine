"""Core helpers for the Koi NNUE Studio.

The tkinter GUI, the headless ``--run`` mode and the self-test all go through
this module, so the whole training pipeline stays testable without a human
clicking anything.

Design notes
------------
* A "run" is a self-contained directory under ``artifacts/training/runs/``
  holding ``config.json``, ``command.json``, ``train.log``, ``pid.txt``,
  ``exit_code.txt`` and the produced network/metadata/validation files.
* Training is launched through a generated ``run.cmd`` inside the run directory
  which starts the real command detached, records its pid and writes the exit
  code.  That makes runs survive a GUI close and lets the GUI re-attach by
  tailing ``train.log``.
* Backends (``tools/nnue/backends``) only provide the command line and the
  file locations; everything else is backend agnostic, so the PyTorch CPU
  trainers and the bullet GPU trainer share the same GUI, progress parsing and
  validation flow.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_DIR = REPO_ROOT / "tools"
STUDIO_DIR = TOOLS_DIR / "nnue"
TRAINING_DIR = REPO_ROOT / "artifacts" / "training"
RUNS_DIR = TRAINING_DIR / "runs"
DEFAULT_CORPUS = TRAINING_DIR / "labels.txt"
DEFAULT_POSITIONS = TRAINING_DIR / "positions.txt"
DEFAULT_BUILD_DIR = REPO_ROOT / "build" / "release"
DEFAULT_BENCH = DEFAULT_BUILD_DIR / "koi-bench.exe"
DEFAULT_ENGINE = DEFAULT_BUILD_DIR / "koi-engine.exe"
RUN_SCHEMA = "koi-nnue-studio-run-v1"
STUDIO_VERSION = "1.0.0"

# The tail thread and the Tk thread both persist run state; serialize the
# read-modify-write so a progress update cannot clobber a finished marker.
_STATE_LOCK = threading.Lock()

# The trainer prints one of these lines per epoch; the GUI parses them to drive
# the progress bar and the validation-MAE chart.  A new backend must emit the
# same information (its adapter may translate its own output).
_EPOCH_RE = re.compile(
    r"^epoch\s+(?P<epoch>\d+)/(?P<epochs>\d+)\s+train_loss\s+(?P<train_loss>[\d.]+)"
    r"\s+val_loss\s+(?P<val_loss>[\d.]+)\s+val_mae_cp\s+(?P<val_mae>[\d.]+)"
    r"(?:\s+time\s+(?P<time>[\d.]+)s)?"
)
_QUANT_RE = re.compile(r"^quantization\s+.*val_mae_cp\s+(?P<val_mae>[\d.]+)")
_SELECTED_RE = re.compile(r"^selected\s+.*val_mae_cp\s+(?P<val_mae>[\d.]+)")
_WROTE_RE = re.compile(r"^wrote\s+(?P<net>\S+)\s+\((?P<bytes>\d+)\s+bytes\)\s+and\s+(?P<meta>\S+)")
_LOADED_RE = re.compile(r"^loaded\s+(?P<rows>\d+)\s+rows?\b.*?\bin\s+(?P<seconds>[\d.]+)s")
_ERROR_RE = re.compile(r"error|traceback|failed|reject", re.IGNORECASE)


def presets() -> dict[str, dict[str, Any]]:
    """Named training presets shown in the GUI and used by ``train.ps1``."""
    return {
        "quick": {"epochs": 1, "rows": 200_000, "batch_size": 4096, "learning_rate": 0.002},
        "standard": {"epochs": 10, "rows": 0, "batch_size": 4096, "learning_rate": 0.002},
        "thorough": {"epochs": 30, "rows": 0, "batch_size": 8192, "learning_rate": 0.0015},
    }


def default_config() -> dict[str, Any]:
    return {
        "backend": "koi",
        "corpus": str(DEFAULT_CORPUS),
        "epochs": 10,
        "batch_size": 4096,
        "learning_rate": 0.002,
        "threads": max(1, (os.cpu_count() or 4) - 1),
        "rows": 0,
        "val_fraction": 0.05,
        "seed": 20260916,
        "format": "v3",
        "hidden_shift": 7,
        "bottleneck_shift": 7,
        "output_shifts": [3, 4, 5, 6],
        "koi_hidden_units": 1024,
        "koi_hidden_shifts": [6, 7, 8],
        "koi_output_shifts": [12, 14, 16, 18, 20],
        "bullet_hidden_units": 1024,
        "bullet_superbatches": 100,
        "bullet_save_rate": 10,
        "bullet_final_learning_rate": 0.0002,
        "net_name": "koi.nnue",
        "float_out": True,
    }


def python_executable() -> str:
    """Interpreter used for the trainer (never ``pythonw``, so output is real)."""
    executable = Path(sys.executable)
    if executable.name.lower() == "pythonw.exe":
        console = executable.with_name("python.exe")
        if console.exists():
            return str(console)
    return str(executable)


def powershell_executable() -> str:
    for candidate in ("pwsh.exe", "powershell.exe"):
        found = shutil.which(candidate)
        if found:
            return found
    return "powershell.exe"


def count_rows(path: Path | str) -> int:
    """Number of lines in a corpus, without loading it into memory."""
    try:
        with open(path, "rb") as handle:
            total = 0
            while True:
                chunk = handle.read(1 << 20)
                if not chunk:
                    return total
                total += chunk.count(b"\n")
    except OSError:
        return 0


def parse_int(text: str, name: str, minimum: int | None = None, maximum: int | None = None) -> int:
    """Parse a GUI/CLI integer with a field-named error message."""
    try:
        value = int(str(text).strip())
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be a whole number") from None
    if minimum is not None and value < minimum:
        raise ValueError(f"{name} must be at least {minimum}")
    if maximum is not None and value > maximum:
        raise ValueError(f"{name} must be at most {maximum}")
    return value


def parse_float(text: str, name: str, minimum: float | None = None, maximum: float | None = None) -> float:
    """Parse a GUI/CLI float with a field-named error message."""
    try:
        value = float(str(text).strip())
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be a number") from None
    if minimum is not None and value < minimum:
        raise ValueError(f"{name} must be at least {minimum}")
    if maximum is not None and value > maximum:
        raise ValueError(f"{name} must be at most {maximum}")
    return value


def parse_progress(line: str) -> dict[str, Any] | None:
    """Translate one trainer log line into a progress event (or ``None``)."""
    line = line.rstrip("\r\n")
    match = _EPOCH_RE.match(line)
    if match:
        return {
            "kind": "epoch",
            "epoch": int(match["epoch"]),
            "epochs": int(match["epochs"]),
            "train_loss": float(match["train_loss"]),
            "val_loss": float(match["val_loss"]),
            "val_mae_cp": float(match["val_mae"]),
            "seconds": float(match["time"]) if match["time"] else None,
        }
    match = _QUANT_RE.match(line)
    if match:
        return {"kind": "quantization", "val_mae_cp": float(match["val_mae"])}
    match = _SELECTED_RE.match(line)
    if match:
        return {"kind": "selected", "val_mae_cp": float(match["val_mae"])}
    match = _WROTE_RE.match(line)
    if match:
        return {
            "kind": "wrote",
            "net": match["net"],
            "bytes": int(match["bytes"]),
            "metadata": match["meta"],
        }
    match = _LOADED_RE.match(line)
    if match:
        seconds = float(match["seconds"])
        return {
            "kind": "loaded",
            "rows": int(match["rows"]),
            "seconds": seconds,
            "rows_per_second": int(match["rows"]) / seconds if seconds > 0 else None,
        }
    return None


# ---------------------------------------------------------------------------
# Progress telemetry
# ---------------------------------------------------------------------------
# ``update_from_log_events`` persists the per-epoch histories the trainer
# prints; the helpers below turn that state into display strings and chart
# series.  They are pure functions so the headless tests can cover them.


def format_duration(seconds: float | None) -> str:
    """Human-readable duration such as ``12s``, ``4m 32s`` or ``1h 05m``."""
    if seconds is None:
        return "unknown"
    total = max(0, int(round(seconds)))
    if total < 60:
        return f"{total}s"
    if total < 3600:
        return f"{total // 60}m {total % 60:02d}s"
    return f"{total // 3600}h {(total % 3600) // 60:02d}m"


def estimate_eta(progress: dict[str, Any]) -> float | None:
    """Seconds until the configured epochs finish (``None`` without timings)."""
    epochs = int(progress.get("epochs", 0) or 0)
    epoch = int(progress.get("epoch", 0) or 0)
    if epochs <= 0:
        return None
    if epoch >= epochs:
        return 0.0
    seconds = [float(value) for value in progress.get("seconds_history", []) if value is not None]
    if not seconds:
        return None
    return (sum(seconds) / len(seconds)) * (epochs - epoch)


def progress_summary(progress: dict[str, Any]) -> str:
    """One-line progress text for the Train tab (epoch, MAE, ETA, throughput)."""
    epochs = int(progress.get("epochs", 0) or 0) or 1
    epoch = int(progress.get("epoch", 0) or 0)
    parts = [f"epoch {epoch}/{epochs}"]
    if progress.get("val_mae_cp") is not None:
        parts.append(f"val MAE {float(progress['val_mae_cp']):.1f} cp")
    eta = estimate_eta(progress)
    if eta:
        parts.append(f"ETA {format_duration(eta)}")
    rows_per_second = progress.get("rows_per_second")
    if rows_per_second:
        parts.append(f"{float(rows_per_second):,.0f} rows/s")
    return " - ".join(parts)


def chart_series(progress: dict[str, Any]) -> list[dict[str, Any]]:
    """Named, colored series for the validation chart (empty series dropped).

    Losses and centipawn error live on different numeric scales, so each series
    carries the axis it belongs to ("loss" or "mae").
    """
    candidates = [
        ("Train loss", "#c0504d", "loss", progress.get("train_loss_history", [])),
        ("Val loss", "#9bbb59", "loss", progress.get("val_loss_history", [])),
        ("Val MAE", "#1f6fb2", "mae", progress.get("history", [])),
    ]
    series = []
    for name, color, axis, values in candidates:
        numbers = [float(value) for value in values if value is not None]
        if numbers:
            series.append({"name": name, "color": color, "axis": axis, "values": numbers})
    return series


def chart_bounds(
    series: list[dict[str, Any]], axis: str | None = None
) -> tuple[float, float] | None:
    """Low/high bounds across the selected series, padded when flat.

    ``axis`` filters to one scale (``"loss"`` or ``"mae"``); ``None`` uses every
    series.  Returns ``None`` when there are no values.
    """
    items = series if axis is None else [item for item in series if item.get("axis") == axis]
    values = [value for item in items for value in item.get("values", [])]
    if not values:
        return None
    low = min(values)
    high = max(values)
    if high - low < 1e-9:
        return low - 1.0, high + 1.0
    return low, high


def log_line_matches(line: str, needle: str = "", errors_only: bool = False) -> bool:
    """Whether a log line passes the Train-tab filter."""
    if errors_only and not _ERROR_RE.search(line):
        return False
    if needle and needle.lower() not in line.lower():
        return False
    return True


# ---------------------------------------------------------------------------
# Network file naming
# ---------------------------------------------------------------------------
# ``net_name`` is the name the trainer writes inside the run directory.  Early
# studio runs ignored it and always wrote ``net.nnue``; the helpers below prefer
# the configured name but fall back to the legacy file when that is what exists,
# so old run directories keep resolving.


def network_file_name(config: dict[str, Any] | None) -> str:
    raw = str((config or {}).get("net_name") or "").strip()
    name = Path(raw).name if raw else ""
    return name or "net.nnue"


def network_path(directory: Path | str, config: dict[str, Any] | None = None) -> Path:
    directory = Path(directory)
    configured = directory / network_file_name(config)
    if configured.exists():
        return configured
    legacy = directory / "net.nnue"
    if legacy.exists() and legacy != configured:
        return legacy
    return configured


def metadata_file_name(config: dict[str, Any] | None) -> str:
    return f"{Path(network_file_name(config)).stem}.metadata.json"


def metadata_path(directory: Path | str, config: dict[str, Any] | None = None) -> Path:
    net = network_path(directory, config)
    return net.with_name(f"{net.stem}.metadata.json")


# ---------------------------------------------------------------------------
# Run directories
# ---------------------------------------------------------------------------


def run_stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


@dataclass
class Run:
    """A training/data run directory with its persisted state."""

    directory: Path
    config: dict[str, Any] = field(default_factory=dict)
    state: dict[str, Any] = field(default_factory=dict)

    @property
    def net_path(self) -> Path:
        return network_path(self.directory, self.config)

    @property
    def metadata_path(self) -> Path:
        return metadata_path(self.directory, self.config)

    @property
    def log_path(self) -> Path:
        return self.directory / "train.log"

    @property
    def pid_path(self) -> Path:
        return self.directory / "pid.txt"

    @property
    def exit_code_path(self) -> Path:
        return self.directory / "exit_code.txt"

    @property
    def lock_path(self) -> Path:
        return self.directory / "running.lock"

    @property
    def status(self) -> str:
        return str(self.state.get("status", "unknown"))

    def pid(self) -> int | None:
        try:
            return int(self.pid_path.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            return None

    def is_running(self) -> bool:
        if not self.lock_path.exists():
            return False
        return pid_alive(self.pid())

    def tail(self, offset: int) -> tuple[list[str], int]:
        return tail_lines(self.log_path, offset)

    def write_state(self, **updates: Any) -> None:
        with _STATE_LOCK:
            self.state.update(updates)
            self.state["schema"] = RUN_SCHEMA
            write_json(self.directory / "state.json", self.state)

    def update_from_log_events(self, events: Iterable[dict[str, Any]]) -> None:
        progress = dict(self.state.get("progress", {}))
        for event in events:
            kind = event.get("kind")
            if kind == "epoch":
                seconds = event.get("seconds")
                progress.update(
                    epoch=event["epoch"],
                    epochs=event["epochs"],
                    val_mae_cp=event["val_mae_cp"],
                    history=progress.get("history", [])
                    + [event["val_mae_cp"]],
                    train_loss_history=progress.get("train_loss_history", [])
                    + [event["train_loss"]],
                    val_loss_history=progress.get("val_loss_history", [])
                    + [event["val_loss"]],
                )
                if seconds is not None:
                    progress["seconds_history"] = progress.get("seconds_history", []) + [
                        float(seconds)
                    ]
            elif kind in {"quantization", "selected"}:
                progress["quantized_val_mae_cp"] = event["val_mae_cp"]
            elif kind == "wrote":
                progress["net_bytes"] = event["bytes"]
            elif kind == "loaded":
                progress["rows_loaded"] = event["rows"]
                if event.get("rows_per_second"):
                    progress["rows_per_second"] = event["rows_per_second"]
        if progress:
            self.write_state(progress=progress)


def create_run(kind: str, config: dict[str, Any], backend_name: str | None = None) -> Run:
    """Create a fresh run directory and persist its configuration."""
    RUNS_DIR.mkdir(parents=True, exist_ok=True)
    label = backend_name or config.get("backend", "torch")
    stamp = run_stamp()
    directory = RUNS_DIR / f"{stamp}-{kind}-{label}"
    suffix = 1
    while directory.exists():
        suffix += 1
        directory = RUNS_DIR / f"{stamp}-{kind}-{label}-{suffix}"
    directory.mkdir(parents=True)
    run = Run(directory=directory, config=dict(config))
    write_json(directory / "config.json", config)
    run.write_state(
        kind=kind,
        backend=label,
        created=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        status="created",
        run=str(directory),
    )
    return run


def load_run(directory: Path | str) -> Run:
    directory = Path(directory)
    return Run(
        directory=directory,
        config=read_json(directory / "config.json", {}),
        state=read_json(directory / "state.json", {}),
    )


def list_runs() -> list[Run]:
    if not RUNS_DIR.exists():
        return []
    runs = [load_run(path) for path in RUNS_DIR.iterdir() if path.is_dir()]
    return sorted(runs, key=lambda run: run.directory.name, reverse=True)


def run_duration_seconds(state: dict[str, Any]) -> float | None:
    """Wall-clock seconds between the ``created`` and ``finished`` stamps."""
    created = state.get("created")
    finished = state.get("finished")
    if not created or not finished:
        return None
    try:
        start = datetime.strptime(str(created), "%Y-%m-%dT%H:%M:%SZ")
        end = datetime.strptime(str(finished), "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        return None
    return max(0.0, (end - start).total_seconds())


def filter_runs(runs: Iterable[Run], text: str = "") -> list[Run]:
    """Runs whose name, kind or status contains ``text`` (case-insensitive)."""
    needle = (text or "").strip().lower()
    if not needle:
        return list(runs)
    return [
        run
        for run in runs
        if needle in run.directory.name.lower()
        or needle in str(run.state.get("kind", "")).lower()
        or needle in str(run.state.get("status", "")).lower()
    ]


def sort_runs(runs: Iterable[Run], key: str = "run", descending: bool = False) -> list[Run]:
    """Sort runs by ``run``, ``kind``, ``status``, ``val_mae`` or ``duration``."""
    def sort_value(run: Run):
        if key == "kind":
            return str(run.state.get("kind", ""))
        if key == "status":
            return str(run.state.get("status", ""))
        if key == "val_mae":
            value = run.state.get("progress", {}).get("val_mae_cp")
            return float(value) if value is not None else float("inf")
        if key == "duration":
            seconds = run_duration_seconds(run.state)
            return seconds if seconds is not None else float("inf")
        return run.directory.name

    return sorted(runs, key=sort_value, reverse=descending)


def failure_lines(run: Run, limit: int = 20) -> list[str]:
    """Last non-empty lines of ``train.err`` for a failed run."""
    error_path = run.directory / "train.err"
    if not error_path.exists():
        return []
    try:
        text = error_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    lines = [line for line in text.splitlines() if line.strip()]
    return lines[-limit:] if limit > 0 else lines


def run_detail_lines(run: Run) -> list[str]:
    """Human-readable summary for the Runs-tab detail pane."""
    state = run.state
    progress = state.get("progress", {})
    duration = run_duration_seconds(state)
    net = run.net_path
    lines = [
        f"run: {run.directory.name}",
        f"kind: {state.get('kind', '?')}   backend: {state.get('backend', '?')}",
        f"status: {state.get('status', 'unknown')}   exit code: {state.get('exit_code', '-')}",
        f"created: {state.get('created', '?')}   finished: {state.get('finished', '-')}",
        f"duration: {format_duration(duration)}   epochs: {progress.get('epoch', 0)}/{progress.get('epochs', 0)}",
        f"val MAE: {progress.get('val_mae_cp', '-')} cp",
        f"network: {net if net.exists() else 'not written'}",
    ]
    errors = failure_lines(run)
    if errors:
        lines.append("errors:")
        lines.extend(f"  {line}" for line in errors)
    return lines


# ---------------------------------------------------------------------------
# Launching, stopping, tailing
# ---------------------------------------------------------------------------


def build_command_file(run: Run, argv: list[str]) -> Path:
    """Persist the exact command line so a run can be inspected or replayed.

    ``argv`` is the complete command: ``argv[0]`` is the executable.
    """
    document = {
        "cwd": str(REPO_ROOT),
        "argv": argv,
        "command": subprocess.list2cmdline(argv),
    }
    path = run.directory / "command.txt"
    path.write_text(document["command"] + "\n", encoding="utf-8")
    write_json(run.directory / "command.json", document)
    return path


def launch_run(run: Run) -> None:
    """Start the run detached so it survives closing the studio window.

    The launcher is a generated ``run.cmd`` inside the run directory: it
    redirects stdout/stderr into ``train.log``/``train.err``, records the exit
    code and finally removes ``running.lock``.  ``pid.txt`` holds the launcher
    process so ``stop_run`` can kill the whole tree.
    """
    command = read_json(run.directory / "command.json", {})
    command_line = command.get("command")
    if not command_line:
        raise RuntimeError("run has no command.json; nothing to launch")
    log = run.directory / "train.log"
    error_log = run.directory / "train.err"
    exit_code = run.exit_code_path
    lock = run.lock_path
    script = run.directory / "run.cmd"
    script.write_text(
        "@echo off\r\n"
        f'cd /d "{command.get("cwd", REPO_ROOT)}"\r\n'
        f"{command_line} > \"{log}\" 2> \"{error_log}\"\r\n"
        f'echo %ERRORLEVEL% > "{exit_code}"\r\n'
        f'del "{lock}" >nul 2>nul\r\n',
        encoding="utf-8",
    )
    lock.write_text("starting\n", encoding="utf-8")
    creationflags = 0
    if os.name == "nt":
        creationflags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(
            subprocess, "CREATE_NEW_PROCESS_GROUP", 0
        )
    launcher_log = open(run.directory / "launcher.log", "ab")
    try:
        process = subprocess.Popen(
            ["cmd.exe", "/d", "/c", str(script)],
            cwd=str(command.get("cwd", REPO_ROOT)),
            stdin=subprocess.DEVNULL,
            stdout=launcher_log,
            stderr=launcher_log,
            creationflags=creationflags,
            close_fds=True,
        )
    finally:
        launcher_log.close()
    run.pid_path.write_text(f"{process.pid}\n", encoding="utf-8")
    run.write_state(status="running", pid=process.pid)


def pid_alive(pid: int | None) -> bool:
    if not pid:
        return False
    if os.name == "nt":
        result = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}"],
            capture_output=True,
            text=True,
            check=False,
        )
        return str(pid) in result.stdout
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def stop_run(run: Run) -> bool:
    """Terminate a run's process tree; returns True when something was killed."""
    pid = run.pid()
    if pid and pid_alive(pid):
        subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            capture_output=True,
            check=False,
        )
    run.lock_path.unlink(missing_ok=True)
    run.write_state(status="stopped", stopped=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    return bool(pid)


def tail_lines(path: Path, offset: int) -> tuple[list[str], int]:
    """Read complete new lines from ``path`` starting at byte ``offset``."""
    if not path.exists():
        return [], offset
    size = path.stat().st_size
    if size < offset:
        offset = 0
    with open(path, "rb") as handle:
        handle.seek(offset)
        chunk = handle.read()
        new_offset = handle.tell()
    if not chunk:
        return [], new_offset
    last_newline = chunk.rfind(b"\n")
    if last_newline < 0:
        return [], offset
    complete = chunk[: last_newline + 1]
    new_offset = offset + last_newline + 1
    return complete.decode("utf-8", errors="replace").splitlines(), new_offset


def refresh_state(run: Run) -> dict[str, Any]:
    """Update the run status from the lock/exit-code files."""
    if run.is_running():
        if run.status != "running":
            run.write_state(status="running")
        return run.state
    if run.exit_code_path.exists():
        try:
            code = int(run.exit_code_path.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            code = -1
        if run.status not in {"completed", "failed", "stopped"}:
            run.write_state(status="completed" if code == 0 else "failed", exit_code=code)
    elif run.status == "running":
        run.write_state(status="unknown")
    return run.state


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------


def run_gate(net: Path, bench: Path | None = None, timeout: int = 900) -> dict[str, Any]:
    """Run ``koi-bench --nnue <net>`` and summarise the 64-position gate."""
    bench = bench or DEFAULT_BENCH
    if not bench.exists():
        raise FileNotFoundError(f"koi-bench not found at {bench}")
    if not net.exists():
        raise FileNotFoundError(f"network not found at {net}")
    result = subprocess.run(
        [str(bench), "--nnue", str(net)],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )
    matches = 0
    total = 0
    failed: list[str] = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields or fields[0] != "position":
            continue
        total += 1
        identifier = fields[1] if len(fields) > 1 else f"position-{total}"
        match = re.search(r"\bmatch\s+([01])\b", line)
        if match and match.group(1) == "1":
            matches += 1
        else:
            failed.append(identifier)
    rejected = "rejected" in result.stderr.lower()
    return {
        "kind": "gate",
        "net": str(net),
        "bench": str(bench),
        "positions": total,
        "matches": matches,
        "failed": failed,
        "rejected": rejected,
        "exit_code": result.returncode,
        "stderr": result.stderr.strip(),
    }


def gate_allows_install(gate: dict[str, Any] | None) -> bool:
    """True only when the 64-position gate actually ran and accepted the net."""
    if not gate or gate.get("error") or gate.get("rejected"):
        return False
    try:
        return int(gate.get("positions", 0)) > 0
    except (TypeError, ValueError):
        return False


def run_ab_match(
    net: Path,
    games: int = 20,
    nodes: int = 20_000,
    threads: int = 1,
    timeout: int = 3_600,
    output_directory: Path | None = None,
) -> dict[str, Any]:
    """Play NNUE-Koi against classical-Koi through ``ab_match.ps1``."""
    script = STUDIO_DIR / "ab_match.ps1"
    if not script.exists():
        raise FileNotFoundError(f"missing match script: {script}")
    report = output_directory or (net.parent / "ab-match.json")
    result = subprocess.run(
        [
            powershell_executable(),
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            "-NnueNet",
            str(net),
            "-Games",
            str(games),
            "-Nodes",
            str(nodes),
            "-Threads",
            str(threads),
            "-ReportPath",
            str(report),
        ],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
        timeout=timeout,
    )
    if report.exists():
        parsed = read_json(report, {})
        parsed["exit_code"] = result.returncode
        return parsed
    return {
        "kind": "ab-match",
        "error": "the match harness produced no report",
        "exit_code": result.returncode,
        "stdout": result.stdout[-4000:],
        "stderr": result.stderr[-4000:],
    }


def validate_net(
    net: Path,
    run: Run | None = None,
    games: int = 20,
    nodes: int = 20_000,
    report_path: Path | None = None,
    gate_only: bool = False,
) -> dict[str, Any]:
    """Gate + A/B match, persisted next to the network.

    ``gate_only`` runs just the 64-position gate; the A/B match is skipped
    entirely (it requires at least two games and is a separate action).
    """
    validation: dict[str, Any] = {"schema": "koi-nnue-studio-validation-v1"}
    try:
        validation["gate"] = run_gate(net)
    except Exception as error:  # noqa: BLE001 - surfaced in the report
        validation["gate"] = {"kind": "gate", "error": str(error)}
    if not gate_only:
        try:
            validation["ab_match"] = run_ab_match(
                net, games=games, nodes=nodes, output_directory=report_path
            )
        except Exception as error:  # noqa: BLE001 - surfaced in the report
            validation["ab_match"] = {"kind": "ab-match", "error": str(error)}
    if run is not None:
        run.write_state(validation=validation)
    return validation


def install_net(net: Path, engine_dir: Path | None = None) -> dict[str, Any]:
    """Copy ``net`` beside the engine, backing up any previous network."""
    engine_dir = engine_dir or DEFAULT_BUILD_DIR
    engine_dir.mkdir(parents=True, exist_ok=True)
    target = engine_dir / "koi.nnue"
    backup: Path | None = None
    if target.exists():
        backup = engine_dir / f"koi.nnue.{time.strftime('%Y%m%d-%H%M%S')}.bak"
        shutil.copy2(target, backup)
    shutil.copy2(net, target)
    return {
        "installed": str(target),
        "backup": str(backup) if backup else None,
        "source": str(net),
        "bytes": target.stat().st_size,
    }


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def read_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return default


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def format_bytes(count: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if count < 1024 or unit == "GB":
            return f"{count:.0f} {unit}" if unit == "B" else f"{count:.1f} {unit}"
        count /= 1024.0
    return f"{count:.1f} GB"
