"""Measure UCI throughput at fixed depth or movetime; this is not a strength test."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import queue
import shutil
import subprocess
import sys
import threading
import time
from typing import Any


SCHEMA = "koi-uci-benchmark-v2"
THREAD_COUNTS = (1, 4)
_EOF = object()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_corpus(path: Path) -> list[str]:
    data = path.read_bytes()
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ValueError(f"FEN corpus is not UTF-8: {path}") from exc
    positions: list[str] = []
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "|" in line:
            name, _, fen = line.partition("|")
            if not name.strip() or not fen.strip():
                raise ValueError(f"malformed named FEN at {path}:{line_number}")
            line = fen.strip()
        positions.append(line)
    if not positions:
        raise ValueError(f"no FEN positions in corpus: {path}")
    return positions


def _ram_bytes() -> int | None:
    if os.name == "nt":
        class MemoryStatus(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong),
                        ("ullTotalPhys", ctypes.c_ulonglong), ("ullAvailPhys", ctypes.c_ulonglong),
                        ("ullTotalPageFile", ctypes.c_ulonglong), ("ullAvailPageFile", ctypes.c_ulonglong),
                        ("ullTotalVirtual", ctypes.c_ulonglong), ("ullAvailVirtual", ctypes.c_ulonglong),
                        ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]

        status = MemoryStatus()
        status.dwLength = ctypes.sizeof(status)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return int(status.ullTotalPhys)
        return None
    if hasattr(os, "sysconf"):
        try:
            return int(os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES"))
        except (ValueError, OSError):
            return None
    return None


def _host_info() -> dict[str, Any]:
    return {
        "cpu": platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER") or platform.machine(),
        "logical_cpu_count": os.cpu_count(),
        "os": platform.platform(),
        "ram_bytes": _ram_bytes(),
    }


class _PeakRss:
    """Poll the OS high-water RSS counter while a child is alive."""

    def __init__(self, pid: int):
        self.pid = pid
        self.value: int | None = None
        self.supported = sys.platform.startswith("linux") or os.name == "nt"
        self.method = "linux_proc_vm_hwm" if sys.platform.startswith("linux") else (
            "windows_peak_working_set" if os.name == "nt" else None)
        self._handle = None
        if os.name == "nt":
            self._open_windows_handle()
        self._stop = threading.Event()
        self._sample()
        self._thread = threading.Thread(target=self._sample_loop, daemon=True)
        self._thread.start()

    def _open_windows_handle(self) -> None:
        try:
            kernel32 = ctypes.windll.kernel32
            kernel32.OpenProcess.restype = ctypes.c_void_p
            self._handle = kernel32.OpenProcess(0x1000 | 0x0010, False, self.pid)
        except (AttributeError, OSError):
            self.supported = False
            self.method = None

    def _sample(self) -> None:
        if sys.platform.startswith("linux"):
            try:
                for line in Path(f"/proc/{self.pid}/status").read_text(encoding="ascii").splitlines():
                    if line.startswith("VmHWM:"):
                        self.value = int(line.split()[1]) * 1024
                        return
            except (FileNotFoundError, PermissionError, ValueError, OSError):
                return
        elif os.name == "nt" and self._handle:
            class ProcessMemoryCounters(ctypes.Structure):
                _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                            ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                            ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            try:
                if ctypes.windll.psapi.GetProcessMemoryInfo(
                    self._handle, ctypes.byref(counters), counters.cb
                ):
                    self.value = int(counters.PeakWorkingSetSize)
            except (AttributeError, OSError):
                self.supported = False
                self.method = None

    def _sample_loop(self) -> None:
        while not self._stop.is_set():
            self._sample()
            self._stop.wait(0.01)

    def close(self) -> dict[str, Any]:
        self._sample()
        self._stop.set()
        self._thread.join(timeout=1)
        if self._handle:
            try:
                ctypes.windll.kernel32.CloseHandle(self._handle)
            except (AttributeError, OSError):
                pass
        return {"bytes": self.value, "supported": self.supported, "method": self.method}


def _resolve_executable(command: list[str]) -> Path:
    candidate = Path(command[0])
    found = str(candidate) if candidate.is_file() else shutil.which(command[0])
    if not found:
        raise FileNotFoundError(f"engine executable not found: {command[0]}")
    return Path(found).resolve()


def _read_lines(process: subprocess.Popen[str], lines: queue.Queue[Any]) -> None:
    assert process.stdout is not None
    try:
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
    finally:
        lines.put(_EOF)


def _next_line(process: subprocess.Popen[str], lines: queue.Queue[Any], timeout: float) -> str:
    try:
        item = lines.get(timeout=timeout)
    except queue.Empty as exc:
        raise TimeoutError("timed out waiting for UCI engine output") from exc
    if item is _EOF:
        raise RuntimeError(f"UCI engine exited before completing the command (exit {process.poll()})")
    return str(item)


def _next_before_deadline(process: subprocess.Popen[str], lines: queue.Queue[Any], deadline: float) -> str:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("timed out waiting for UCI engine output")
    return _next_line(process, lines, remaining)


def _send(process: subprocess.Popen[str], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write(command + "\n")
    process.stdin.flush()


def _handshake(process: subprocess.Popen[str], lines: queue.Queue[Any], timeout: float
               ) -> tuple[dict[str, str | None], list[dict[str, Any]]]:
    _send(process, "uci")
    identity: dict[str, str | None] = {"name": None, "author": None}
    options: list[dict[str, Any]] = []
    deadline = time.monotonic() + timeout
    while True:
        line = _next_before_deadline(process, lines, deadline)
        if line.startswith("id name "):
            identity["name"] = line[8:]
        elif line.startswith("id author "):
            identity["author"] = line[10:]
        elif line.startswith("option name ") and " type " in line:
            definition = line[len("option name "):]
            option_name, _, remainder = definition.partition(" type ")
            tokens = remainder.split()
            if not tokens:
                continue

            def integer_after(key: str) -> int | None:
                try:
                    return int(tokens[tokens.index(key) + 1])
                except (ValueError, IndexError):
                    return None

            options.append({
                "name": option_name,
                "type": tokens[0],
                "min": integer_after("min"),
                "max": integer_after("max"),
            })
        elif line == "uciok":
            return identity, options


def _ready(process: subprocess.Popen[str], lines: queue.Queue[Any], timeout: float) -> None:
    _send(process, "isready")
    deadline = time.monotonic() + timeout
    while True:
        if _next_before_deadline(process, lines, deadline) == "readyok":
            return


def _parse_info(line: str) -> dict[str, int | None] | None:
    parts = line.split()
    if not parts or parts[0] != "info":
        return None
    values: dict[str, int] = {}
    for key, field in (("depth", "depth"), ("nodes", "nodes"), ("nps", "nps"), ("time", "time_ms")):
        try:
            values[field] = int(parts[parts.index(key) + 1])
        except (ValueError, IndexError):
            pass
    if "depth" not in values:
        return None
    return {key: values.get(key) for key in ("depth", "nodes", "nps", "time_ms")}


def _search_position(process: subprocess.Popen[str], lines: queue.Queue[Any], fen: str,
                     go_command: str, timeout: float,
                     movetime_ms: int | None = None) -> dict[str, Any]:
    _send(process, "ucinewgame")
    _ready(process, lines, timeout)
    _send(process, f"position fen {fen}")
    started = time.monotonic()
    _send(process, go_command)
    search_deadline = started + (movetime_ms / 1000.0 if movetime_ms is not None else timeout)
    stop_sent_at: float | None = None
    depth_rows: dict[int, dict[str, int | None]] = {}
    bestmove: str | None = None
    ponder: str | None = None
    bestmove_at: float | None = None
    while bestmove is None:
        now = time.monotonic()
        if movetime_ms is not None and stop_sent_at is None and now >= search_deadline:
            stop_sent_at = now
            _send(process, "stop")
            continue
        deadline = search_deadline if stop_sent_at is None else stop_sent_at + timeout
        remaining = deadline - now
        if remaining <= 0:
            if movetime_ms is not None and stop_sent_at is None:
                stop_sent_at = time.monotonic()
                _send(process, "stop")
                continue
            if stop_sent_at is not None:
                raise TimeoutError("timed out waiting for bestmove after stop")
            raise TimeoutError("timed out waiting for UCI engine output")
        try:
            line = _next_line(process, lines, remaining)
        except TimeoutError as exc:
            if movetime_ms is not None and stop_sent_at is None:
                stop_sent_at = time.monotonic()
                _send(process, "stop")
                continue
            if stop_sent_at is not None:
                raise TimeoutError("timed out waiting for bestmove after stop") from exc
            raise
        if line.casefold().startswith("info string invalid position"):
            raise RuntimeError(f"engine rejected FEN: {fen}")
        row = _parse_info(line)
        if row:
            row_depth = int(row["depth"])
            previous = depth_rows.get(row_depth)
            row_complete = all(row[field] is not None for field in ("nodes", "nps", "time_ms"))
            previous_complete = previous is not None and all(
                previous[field] is not None for field in ("nodes", "nps", "time_ms"))
            if previous is None or row_complete:
                depth_rows[row_depth] = row
            elif not previous_complete:
                depth_rows[row_depth] = {
                    field: row[field] if row[field] is not None else previous[field]
                    for field in ("depth", "nodes", "nps", "time_ms")
                }
        elif line.startswith("bestmove "):
            tokens = line.split()
            bestmove = tokens[1] if len(tokens) > 1 else None
            bestmove_at = time.monotonic()
            if len(tokens) >= 4 and tokens[2] == "ponder":
                ponder = tokens[3]
    assert bestmove_at is not None
    search_finished_at = stop_sent_at if stop_sent_at is not None else bestmove_at
    return {
        "fen": fen,
        "go_command": go_command,
        "requested_movetime_ms": movetime_ms,
        "stop_sent": stop_sent_at is not None,
        "search_elapsed_ms": int(round((search_finished_at - started) * 1000)),
        "stop_latency_ms": int(round((bestmove_at - stop_sent_at) * 1000))
        if stop_sent_at is not None else 0,
        "time_to_bestmove_ms": int(round((bestmove_at - started) * 1000)),
        "depth_rows": [depth_rows[key] for key in sorted(depth_rows)],
        "completed_depth": max(depth_rows, default=0),
        "bestmove": bestmove,
        "ponder": ponder,
    }


def _run_one(engine_name: str, command: list[str], executable: Path, threads: int,
             positions: list[str], hash_mb: int, depth: int | None,
             movetime_ms: int | None, timeout: float) -> dict[str, Any]:
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                               errors="replace", bufsize=1)
    rss = _PeakRss(process.pid)
    lines: queue.Queue[Any] = queue.Queue()
    reader = threading.Thread(target=_read_lines, args=(process, lines), daemon=True)
    reader.start()
    try:
        identity, option_specs = _handshake(process, lines, timeout)
        options_by_name = {option["name"].casefold(): option for option in option_specs}
        requested_options = {"Threads": threads, "Hash": hash_mb}
        missing_options = [name for name in requested_options if name.casefold() not in options_by_name]
        if missing_options:
            raise RuntimeError(
                f"{engine_name} engine does not advertise required UCI option(s): {', '.join(missing_options)}"
            )
        for name, value in requested_options.items():
            option = options_by_name[name.casefold()]
            if option["type"].casefold() != "spin":
                raise RuntimeError(f"{engine_name} UCI option {name} must be a spin option")
            minimum, maximum = option["min"], option["max"]
            if minimum is None or maximum is None or minimum > maximum:
                raise RuntimeError(f"{engine_name} UCI option {name} must advertise a valid spin range")
            if not minimum <= value <= maximum:
                raise RuntimeError(
                    f"{engine_name} requested {name} value {value} is outside advertised range {minimum}..{maximum}"
                )
        _send(process, f"setoption name Hash value {hash_mb}")
        _send(process, f"setoption name Threads value {threads}")
        _ready(process, lines, timeout)
        go_command = f"go depth {depth}" if depth is not None else "go infinite"
        results = []
        for index, fen in enumerate(positions, start=1):
            try:
                results.append(_search_position(
                    process, lines, fen, go_command, timeout, movetime_ms
                ))
            except TimeoutError as exc:
                raise TimeoutError(
                    f"{engine_name} Threads={threads} position {index}/{len(positions)} timed out: {exc}"
                ) from exc
            except RuntimeError as exc:
                raise RuntimeError(
                    f"{engine_name} Threads={threads} position {index}/{len(positions)} failed: {exc}"
                ) from exc
    finally:
        if process.poll() is None:
            try:
                _send(process, "quit")
            except (BrokenPipeError, OSError):
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        reader.join(timeout=1)
        if process.stdin:
            process.stdin.close()
        if process.stdout:
            process.stdout.close()
        peak_rss = rss.close()
    completed_depth = min((position["completed_depth"] for position in results), default=0)
    return {
        "engine": engine_name,
        "engine_id": identity,
        "advertised_options": [option["name"] for option in option_specs],
        "option_specs": option_specs,
        "executable": {"path": str(executable), "sha256": _sha256(executable)},
        "options": {"Hash": hash_mb, "Threads": threads},
        "peak_rss_bytes": peak_rss["bytes"],
        "peak_rss_supported": peak_rss["supported"],
        "peak_rss_method": peak_rss["method"],
        "completed_depth": completed_depth,
        "positions": results,
    }


def run_benchmark(*, engines: list[tuple[str, list[str]]], corpus_path: Path | str,
                  source_revision: str, output_path: Path | str | None = None,
                  hash_mb: int = 512, depth: int | None = None,
                  movetime_ms: int | None = None,
                  timeout_seconds: float = 30) -> dict[str, Any]:
    """Run each engine at one and four threads using a shared depth or movetime."""
    corpus_path = Path(corpus_path).resolve()
    positions = _read_corpus(corpus_path)
    if not engines:
        raise ValueError("at least one engine is required")
    if depth is None and movetime_ms is None:
        depth = 12
    if depth is not None and movetime_ms is not None:
        raise ValueError("specify either depth or movetime_ms, not both")
    if hash_mb <= 0 or (depth is not None and depth <= 0) or \
            (movetime_ms is not None and movetime_ms <= 0) or timeout_seconds <= 0:
        raise ValueError("Hash, search limit, and timeout must be positive")
    if any(not name or not command for name, command in engines):
        raise ValueError("each engine requires a name and command")

    report: dict[str, Any] = {
        "schema": SCHEMA,
        "description": "UCI throughput measurement only; NPS is intra-engine and this is not an Elo or strength comparison.",
        "source_revision": source_revision,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": _host_info(),
        "corpus": {"path": str(corpus_path), "sha256": _sha256(corpus_path), "position_count": len(positions)},
        "settings": {
            "hash_mb": hash_mb,
            "depth": depth,
            "movetime_ms": movetime_ms,
            "movetime_policy": "external_stop" if movetime_ms is not None else None,
            "threads": list(THREAD_COUNTS),
            "timeout_seconds": timeout_seconds,
        },
        "runs": [],
    }
    # The outer order is deliberate: each engine finishes its 1-thread and
    # 4-thread corpus before the next engine is launched.
    for name, command in engines:
        executable = _resolve_executable(command)
        for threads in THREAD_COUNTS:
            report["runs"].append(_run_one(name, command, executable, threads, positions,
                                            hash_mb, depth, movetime_ms, timeout_seconds))
    if output_path is not None:
        output_path = Path(output_path)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--koi", required=True, help="path to the Koi UCI executable")
    parser.add_argument("--stockfish", required=True, help="path to a Stockfish-style UCI executable")
    parser.add_argument("--fen-corpus", required=True, type=Path, help="UTF-8 file with one FEN per line; # comments are ignored")
    parser.add_argument("--source-revision", required=True, help="source revision recorded in the report")
    parser.add_argument("--output", required=True, type=Path, help="JSON report destination")
    parser.add_argument("--hash-mb", type=int, default=512)
    search_limit = parser.add_mutually_exclusive_group()
    search_limit.add_argument("--depth", type=int, help="search to this depth (default: 12)")
    search_limit.add_argument("--movetime-ms", type=int, help="search each position for this many milliseconds")
    parser.add_argument("--timeout-seconds", type=float, default=30)
    args = parser.parse_args(argv)
    try:
        run_benchmark(engines=[("koi", [args.koi]), ("stockfish", [args.stockfish])],
                      corpus_path=args.fen_corpus, source_revision=args.source_revision,
                      output_path=args.output, hash_mb=args.hash_mb, depth=args.depth,
                      movetime_ms=args.movetime_ms,
                      timeout_seconds=args.timeout_seconds)
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        parser.exit(2, f"uci_benchmark: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
