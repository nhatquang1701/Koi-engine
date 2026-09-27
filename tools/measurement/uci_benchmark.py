"""Measure UCI search throughput at fixed depth; this is not a strength test."""

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


SCHEMA = "koi-uci-benchmark-v1"
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
    positions = [line.strip() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
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


def _send(process: subprocess.Popen[str], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write(command + "\n")
    process.stdin.flush()


def _handshake(process: subprocess.Popen[str], lines: queue.Queue[Any], timeout: float
               ) -> tuple[dict[str, str | None], list[str]]:
    _send(process, "uci")
    identity: dict[str, str | None] = {"name": None, "author": None}
    options: list[str] = []
    deadline = time.monotonic() + timeout
    while True:
        line = _next_line(process, lines, max(0.001, deadline - time.monotonic()))
        if line.startswith("id name "):
            identity["name"] = line[8:]
        elif line.startswith("id author "):
            identity["author"] = line[10:]
        elif line.startswith("option name ") and " type " in line:
            options.append(line[len("option name "):].split(" type ", 1)[0])
        elif line == "uciok":
            return identity, options


def _ready(process: subprocess.Popen[str], lines: queue.Queue[Any], timeout: float) -> None:
    _send(process, "isready")
    deadline = time.monotonic() + timeout
    while True:
        if _next_line(process, lines, max(0.001, deadline - time.monotonic())) == "readyok":
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
                     depth: int, timeout: float) -> dict[str, Any]:
    _send(process, "ucinewgame")
    _ready(process, lines, timeout)
    _send(process, f"position fen {fen}")
    _send(process, f"go depth {depth}")
    deadline = time.monotonic() + timeout
    depth_rows: dict[int, dict[str, int | None]] = {}
    bestmove: str | None = None
    ponder: str | None = None
    while bestmove is None:
        line = _next_line(process, lines, max(0.001, deadline - time.monotonic()))
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
            if len(tokens) >= 4 and tokens[2] == "ponder":
                ponder = tokens[3]
    return {
        "fen": fen,
        "depth_rows": [depth_rows[key] for key in sorted(depth_rows)],
        "completed_depth": max(depth_rows, default=0),
        "bestmove": bestmove,
        "ponder": ponder,
    }


def _run_one(engine_name: str, command: list[str], executable: Path, threads: int,
             positions: list[str], hash_mb: int, depth: int, timeout: float) -> dict[str, Any]:
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, encoding="utf-8",
                               errors="replace", bufsize=1)
    rss = _PeakRss(process.pid)
    lines: queue.Queue[Any] = queue.Queue()
    reader = threading.Thread(target=_read_lines, args=(process, lines), daemon=True)
    reader.start()
    try:
        identity, advertised_options = _handshake(process, lines, timeout)
        advertised_option_keys = {option.casefold() for option in advertised_options}
        missing_options = [name for name in ("Threads", "Hash") if name.casefold() not in advertised_option_keys]
        if missing_options:
            raise RuntimeError(
                f"{engine_name} engine does not advertise required UCI option(s): {', '.join(missing_options)}"
            )
        _send(process, f"setoption name Hash value {hash_mb}")
        _send(process, f"setoption name Threads value {threads}")
        _ready(process, lines, timeout)
        results = [_search_position(process, lines, fen, depth, timeout) for fen in positions]
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
        "advertised_options": advertised_options,
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
                  hash_mb: int = 512, depth: int = 12, timeout_seconds: float = 30) -> dict[str, Any]:
    """Run each engine, in order, with one and four threads over the same corpus."""
    corpus_path = Path(corpus_path).resolve()
    positions = _read_corpus(corpus_path)
    if not engines:
        raise ValueError("at least one engine is required")
    if hash_mb <= 0 or depth <= 0 or timeout_seconds <= 0:
        raise ValueError("Hash, depth, and timeout must be positive")
    if any(not name or not command for name, command in engines):
        raise ValueError("each engine requires a name and command")

    report: dict[str, Any] = {
        "schema": SCHEMA,
        "description": "UCI throughput measurement only; NPS is intra-engine and this is not an Elo or strength comparison.",
        "source_revision": source_revision,
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": _host_info(),
        "corpus": {"path": str(corpus_path), "sha256": _sha256(corpus_path), "position_count": len(positions)},
        "settings": {"hash_mb": hash_mb, "depth": depth, "threads": list(THREAD_COUNTS)},
        "runs": [],
    }
    # The outer order is deliberate: each engine finishes its 1-thread and
    # 4-thread corpus before the next engine is launched.
    for name, command in engines:
        executable = _resolve_executable(command)
        for threads in THREAD_COUNTS:
            report["runs"].append(_run_one(name, command, executable, threads, positions,
                                            hash_mb, depth, timeout_seconds))
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
    parser.add_argument("--depth", type=int, default=12)
    parser.add_argument("--timeout-seconds", type=float, default=30)
    args = parser.parse_args(argv)
    try:
        run_benchmark(engines=[("koi", [args.koi]), ("stockfish", [args.stockfish])],
                      corpus_path=args.fen_corpus, source_revision=args.source_revision,
                      output_path=args.output, hash_mb=args.hash_mb, depth=args.depth,
                      timeout_seconds=args.timeout_seconds)
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        parser.exit(2, f"uci_benchmark: {exc}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
