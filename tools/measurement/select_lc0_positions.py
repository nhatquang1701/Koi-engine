"""Select Lc0 curriculum positions from FEN corpora.

Scans one or more FEN-line files (lines start with a FEN; any further
``;``-separated fields are ignored) and reservoir-samples positions into three
curriculum buckets for Lc0 labeling:

* ``endgame``  -- both sides' non-pawn, non-king material <= 13 or total
  pieces <= 12.
* ``tactical`` -- at least one legal move that captures a knight or better,
  or gives check.
* ``general``  -- everything else with ply >= 8 (book-like openings are
  skipped).

Selection is deterministic for a given seed, single-pass over the inputs, and
never holds the whole corpus in memory: only the current reservoir contents
(up to the per-bucket targets) are kept.

Usage:
  python tools/measurement/select_lc0_positions.py \
      --input artifacts/training/koi-v5-official-20261002/corpus/train.txt \
      --out-dir artifacts/training/lc0-curriculum-20261004

Outputs ``general.txt``, ``tactical.txt``, and ``endgame.txt`` (one FEN per
line) plus ``stats.json`` with the ``koi-lc0-curriculum-selection-v1`` schema.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

# Allow direct execution as ``python tools/measurement/select_lc0_positions.py``.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import koi_chess as chess  # noqa: E402  (imported after the sys.path bootstrap)

SCHEMA = "koi-lc0-curriculum-selection-v1"
BUCKETS = ("general", "tactical", "endgame")
PROGRESS_EVERY = 100_000
DEFAULT_INPUT = "artifacts/training/koi-v5-official-20261002/corpus/train.txt"
DEFAULT_OUT_DIR = "artifacts/training/lc0-curriculum-20261004/"

#: Standard centipawn-like piece values; kings count zero.
PIECE_VALUES = {
    chess.PAWN: 1,
    chess.KNIGHT: 3,
    chess.BISHOP: 3,
    chess.ROOK: 5,
    chess.QUEEN: 9,
    chess.KING: 0,
}

#: A capture counts as tactical when the captured piece is worth at least this.
TACTICAL_CAPTURE_VALUE = 3


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def position_ply(board: chess.Board) -> int:
    """Half-move number of the position.

    koi_chess counts only moves pushed in-process, so a board created from a
    FEN reports ply 0; fall back to the FEN's fullmove number and turn the way
    python-chess does so book-like positions can still be recognized.
    """
    ply = board.ply()
    if ply:
        return ply
    return 2 * (board.fullmove_number - 1) + (0 if board.turn == chess.WHITE else 1)


def non_pawn_material(pieces: dict) -> int:
    """Total non-pawn, non-king material value of both sides."""
    return sum(
        PIECE_VALUES[piece.piece_type]
        for piece in pieces.values()
        if piece.piece_type not in (chess.PAWN, chess.KING)
    )


def is_tactical(board: chess.Board, moves: list) -> bool:
    """True when a legal move captures a valuable piece or gives check."""
    for move in moves:
        if board.is_capture(move):
            captured = board.piece_at(move.to_square)
            if captured is not None and PIECE_VALUES[captured.piece_type] >= TACTICAL_CAPTURE_VALUE:
                return True
        if board.gives_check(move):
            return True
    return False


def classify(fen: str):
    """Return ``(bucket, reason)`` for a FEN; bucket is None when skipped."""
    try:
        board = chess.Board(fen)
    except (ValueError, IndexError):
        return None, "invalid_fen"
    if not board.is_valid():
        return None, "invalid"
    moves = board.legal_moves
    if board.is_game_over():
        return None, "no_moves" if not moves else "game_over"

    pieces = board.piece_map()
    if non_pawn_material(pieces) <= 13 or len(pieces) <= 12:
        return "endgame", None
    if is_tactical(board, moves):
        return "tactical", None
    if position_ply(board) < 8:
        return None, "book"
    return "general", None


class Reservoir:
    """Reservoir sample of up to ``target`` strings using algorithm R."""

    __slots__ = ("target", "items", "seen", "rng")

    def __init__(self, target: int, rng: random.Random):
        self.target = target
        self.items: list[str] = []
        self.seen = 0
        self.rng = rng

    def add(self, value: str) -> None:
        self.seen += 1
        if self.target <= 0:
            return
        if len(self.items) < self.target:
            self.items.append(value)
            return
        index = self.rng.randrange(self.seen)
        if index < self.target:
            self.items[index] = value


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sample FEN corpora into Lc0 curriculum buckets.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--input",
        action="append",
        type=Path,
        default=None,
        help="FEN corpus file; repeat for multiple inputs.",
    )
    parser.add_argument("--out-dir", type=Path, default=Path(DEFAULT_OUT_DIR))
    parser.add_argument("--target-general", type=int, default=600_000)
    parser.add_argument("--target-tactical", type=int, default=300_000)
    parser.add_argument("--target-endgame", type=int, default=150_000)
    parser.add_argument("--seed", type=int, default=20261004)
    parser.add_argument(
        "--limit-scan",
        type=int,
        default=0,
        help="Stop after this many input lines (0 scans everything).",
    )
    return parser.parse_args(argv)


def write_bucket(path: Path, items: list[str]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for item in items:
            handle.write(item + "\n")


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    inputs = args.input if args.input else [Path(DEFAULT_INPUT)]
    targets = {
        "general": args.target_general,
        "tactical": args.target_tactical,
        "endgame": args.target_endgame,
    }

    rng = random.Random(args.seed)
    reservoirs = {name: Reservoir(targets[name], rng) for name in BUCKETS}
    skipped_reasons: dict[str, int] = {}
    scanned = 0
    skipped = 0
    start = time.monotonic()

    def report_progress() -> None:
        elapsed = time.monotonic() - start
        rate = scanned / elapsed if elapsed > 0 else 0.0
        message = f"scanned {scanned:,} lines in {elapsed:.1f}s ({rate:,.0f}/s)"
        if args.limit_scan > 0:
            remaining = max(args.limit_scan - scanned, 0)
            eta = remaining / rate if rate > 0 else 0.0
            message += f", eta {eta:.0f}s"
        log(message)

    stop = False
    for input_path in inputs:
        try:
            handle = input_path.open("r", encoding="utf-8", errors="replace")
        except OSError as error:
            log(f"error: cannot open input '{input_path}': {error}")
            return 2
        with handle:
            for raw_line in handle:
                if args.limit_scan > 0 and scanned >= args.limit_scan:
                    stop = True
                    break
                scanned += 1

                text = raw_line.strip()
                if not text:
                    skipped += 1
                    skipped_reasons["empty"] = skipped_reasons.get("empty", 0) + 1
                    continue
                fen = text.split(";", 1)[0].strip()
                if not fen:
                    skipped += 1
                    skipped_reasons["empty_fen"] = skipped_reasons.get("empty_fen", 0) + 1
                    continue

                bucket, reason = classify(fen)
                if bucket is None:
                    skipped += 1
                    skipped_reasons[reason] = skipped_reasons.get(reason, 0) + 1
                else:
                    reservoirs[bucket].add(fen)

                if scanned % PROGRESS_EVERY == 0:
                    report_progress()
        if stop:
            break

    elapsed = time.monotonic() - start
    log(f"scan complete: {scanned:,} lines, {skipped:,} skipped, {elapsed:.1f}s")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    bucket_stats = {}
    for name in BUCKETS:
        reservoir = reservoirs[name]
        output = args.out_dir / f"{name}.txt"
        write_bucket(output, reservoir.items)
        bucket_stats[name] = {
            "target": targets[name],
            "seen": reservoir.seen,
            "sampled": len(reservoir.items),
            "output": str(output),
        }
        log(f"{name}: sampled {len(reservoir.items):,} of {reservoir.seen:,} seen")

    stats = {
        "schema": SCHEMA,
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "seed": args.seed,
        "limit_scan": args.limit_scan,
        "inputs": [str(path) for path in inputs],
        "scanned": scanned,
        "skipped": skipped,
        "skipped_reasons": skipped_reasons,
        "elapsed_seconds": round(elapsed, 3),
        "buckets": bucket_stats,
    }
    stats_path = args.out_dir / "stats.json"
    with stats_path.open("w", encoding="utf-8") as handle:
        json.dump(stats, handle, indent=2)
        handle.write("\n")
    log(f"wrote {stats_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
