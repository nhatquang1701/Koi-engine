"""Label chess positions with Lc0 teacher data (value + policy-ready).

Drives an Lc0 UCI engine over a list of FENs and records, for each position,
the searched root score, the root win-loss estimate, the chosen best move, and
the per-move visit/policy distribution from ``VerboseMoveStats``.  The output
is JSONL (one record per input position, in order, errors included) so the run
is resumable by counting lines.

The visit distribution is captured so a policy head can be trained later
without regenerating the corpus; value-only consumers can ignore it.

Usage:
  python tools/measurement/gen_lc0_labels.py \
      --input artifacts/training/positions.txt \
      --output artifacts/training/lc0-labels.jsonl \
      --lc0 C:/Users/ntATh/koi-run/lc0-v0.32.1-cudnn/lc0.exe \
      --weights C:/Users/ntATh/koi-run/lc0-v0.32.1-cudnn/791556.pb.gz \
      --backend cudnn --nodes 800

Every run appends to the output and skips already-written positions, so the
command can simply be relaunched after an interruption.  Use ``--start-index``
to force a different offset (e.g. after changing the input file).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import queue
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

_SCORE_RE = re.compile(r"\bscore cp (-?\d+)")
_NODE_RE = re.compile(
    r"^info string node\s+\((\s*\d+)\)\s+N:\s*(\d+).*\(WL:\s*(-?[\d.]+)\)\s*\(D:\s*([\d.]+)\)"
)
_STATS_RE = re.compile(
    r"^info string ([a-h][1-8][a-h][1-8][qrbn]?)\s+\(\s*\d+\s*\)\s+N:\s*(\d+)\s+\(\+\s*\d+\)\s+"
    r"\(P:\s*([\d.]+)%\)\s+\(WL:\s*(-?[\d.]+)\)\s+\(D:\s*([\d.]+)\)"
)


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Lc0Engine:
    """Minimal UCI driver for Lc0 with verbose-move-stats parsing."""

    def __init__(self, exe: Path, weights: Path, options: list[tuple[str, str]]):
        self.exe = exe
        self.weights = weights
        self.options = options
        self.proc: subprocess.Popen[str] | None = None
        self.lines: queue.Queue[str | None] = queue.Queue()
        self.reader: threading.Thread | None = None
        self.banner: str = ""

    def start(self) -> None:
        self.stop()
        args = [str(self.exe), f"--weights={self.weights}"]
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        self.send("uci")
        self._wait_for("uciok", timeout=60.0)
        for name, value in self.options:
            self.send(f"setoption name {name} value {value}")
        self.send("isready")
        self._wait_for("readyok", timeout=120.0)

    def _read_loop(self) -> None:
        assert self.proc is not None and self.proc.stdout is not None
        for line in self.proc.stdout:
            self.lines.put(line.rstrip("\n"))
        self.lines.put(None)

    def send(self, command: str) -> None:
        assert self.proc is not None and self.proc.stdin is not None
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()

    def _wait_for(self, token: str, timeout: float) -> list[str]:
        collected: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError(f"engine exited with {self.proc.returncode}")
            try:
                line = self.lines.get(timeout=0.25)
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("engine output closed")
            collected.append(line)
            if token in line:
                return collected
        raise TimeoutError(f"no '{token}' within {timeout:.0f}s")

    def analyze(self, fen: str, nodes: int, timeout: float) -> dict:
        self.send(f"position fen {fen}")
        self.send(f"go nodes {nodes}")
        visits: dict[str, int] = {}
        policy: dict[str, float] = {}
        wl: dict[str, float] = {}
        draw: dict[str, float] = {}
        score_cp: int | None = None
        root_wl: float | None = None
        root_draw: float | None = None
        bestmove: str | None = None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc is not None and self.proc.poll() is not None:
                raise RuntimeError(f"engine exited with {self.proc.returncode}")
            try:
                line = self.lines.get(timeout=0.25)
            except queue.Empty:
                continue
            if line is None:
                raise RuntimeError("engine output closed")
            if line.startswith("bestmove"):
                parts = line.split()
                bestmove = parts[1] if len(parts) > 1 else None
                break
            match = _SCORE_RE.search(line)
            if match and line.startswith("info depth"):
                score_cp = int(match.group(1))
            match = _STATS_RE.match(line)
            if match:
                move, n, p, w, d = match.groups()
                visits[move] = int(n)
                policy[move] = float(p)
                wl[move] = float(w)
                draw[move] = float(d)
                continue
            match = _NODE_RE.match(line)
            if match:
                root_wl = float(match.group(3))
                root_draw = float(match.group(4))
        else:
            raise TimeoutError(f"no bestmove within {timeout:.0f}s")
        if self.banner == "":
            self.banner = f"lc0 {self.exe.name}"
        return {
            "bestmove": bestmove,
            "score_cp": score_cp,
            "root_wl": root_wl,
            "root_draw": root_draw,
            "visits": visits,
            "policy": policy,
            "wl": wl,
            "draw": draw,
        }

    def stop(self) -> None:
        if self.proc is not None:
            try:
                self.send("quit")
                self.proc.wait(timeout=5.0)
            except Exception:
                self.proc.kill()
            finally:
                self.proc = None


def load_fens(path: Path) -> list[str]:
    fens: list[str] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fens.append(line.split(";")[0].strip())
    return fens


def count_lines(path: Path) -> int:
    if not path.exists():
        return 0
    with path.open("r", encoding="utf-8") as handle:
        return sum(1 for _ in handle)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="position file (one FEN per line, optional ';' fields)")
    parser.add_argument("--output", required=True, help="output JSONL (appended, resumable)")
    parser.add_argument("--lc0", required=True, help="path to lc0.exe")
    parser.add_argument("--weights", required=True, help="path to the network file")
    parser.add_argument("--backend", default="cudnn", help="Lc0 backend name")
    parser.add_argument("--nodes", type=int, default=800, help="playouts per position")
    parser.add_argument("--threads", type=int, default=2, help="Lc0 search threads")
    parser.add_argument("--minibatch-size", type=int, default=256)
    parser.add_argument("--max-prefetch", type=int, default=32)
    parser.add_argument("--timeout-seconds", type=float, default=180.0, help="per-position wall budget")
    parser.add_argument("--start-index", type=int, default=-1, help="override resume offset (-1 = from output length)")
    parser.add_argument("--limit", type=int, default=0, help="stop after N positions (0 = all)")
    parser.add_argument("--log-every", type=int, default=100)
    args = parser.parse_args(argv)

    exe = Path(args.lc0).resolve()
    weights = Path(args.weights).resolve()
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fens = load_fens(Path(args.input))
    if not fens:
        log("input has no positions")
        return 2

    done = count_lines(out_path) if args.start_index < 0 else args.start_index
    if done > len(fens):
        log(f"output already has {done} records but input has only {len(fens)} positions; nothing to do")
        return 0
    log(f"{len(fens)} positions, resuming at index {done}")

    options = [
        ("Backend", args.backend),
        ("Threads", str(args.threads)),
        ("MinibatchSize", str(args.minibatch_size)),
        ("MaxPrefetch", str(args.max_prefetch)),
        ("VerboseMoveStats", "true"),
    ]
    engine = Lc0Engine(exe, weights, options)
    engine.start()

    manifest = {
        "schema": "koi-lc0-labels-v1",
        "created_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "input": str(Path(args.input).resolve()),
        "output": str(out_path.resolve()),
        "lc0": str(exe),
        "lc0_sha256": sha256_file(exe),
        "weights": str(weights),
        "weights_sha256": sha256_file(weights),
        "backend": args.backend,
        "nodes": args.nodes,
        "threads": args.threads,
        "minibatch_size": args.minibatch_size,
        "max_prefetch": args.max_prefetch,
        "position_count": len(fens),
    }
    (out_path.with_suffix(out_path.suffix + ".manifest.json")).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    start = time.monotonic()
    processed = 0
    errors = 0
    with out_path.open("a", encoding="utf-8") as out:
        index = done
        while index < len(fens):
            if args.limit and processed >= args.limit:
                break
            fen = fens[index]
            try:
                result = engine.analyze(fen, args.nodes, args.timeout_seconds)
            except Exception as error:  # restart and record the failure
                errors += 1
                log(f"position {index} failed: {error}")
                engine.start()
                record = {"fen": fen, "error": str(error)}
            else:
                record = {"fen": fen, **result}
            record["index"] = index
            out.write(json.dumps(record, separators=(",", ":")) + "\n")
            out.flush()
            index += 1
            processed += 1
            if processed % args.log_every == 0:
                elapsed = time.monotonic() - start
                rate = processed / elapsed if elapsed > 0 else 0.0
                remaining = (len(fens) - index) / rate if rate > 0 else 0.0
                log(
                    f"{index}/{len(fens)} positions, {rate:.2f} pos/s, "
                    f"eta {remaining / 3600:.1f} h, errors {errors}"
                )
    engine.stop()
    elapsed = time.monotonic() - start
    log(f"done: {processed} positions in {elapsed:.1f}s ({errors} errors)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
