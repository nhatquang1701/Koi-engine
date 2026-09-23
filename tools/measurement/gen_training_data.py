"""Generate Stockfish-labeled NNUE training data for Koi.

Two stages, both parallelised across worker threads:

  1. games     - play Stockfish self-play (and optional noisy random) games,
                 extract every position into a deduplicated ``FEN;result`` list
                 where result is the white-relative game outcome (1.0/0.5/0.0).
  2. label     - score each FEN with Stockfish at a fixed depth and write
                 ``FEN;cp;best_move;result`` rows (cp is from the side to move).

The output is consumed by ``train_nnue_koi.py`` (version 4) directly or through
``koi_dataset.py``, and by the legacy ``train_nnue_sf.py`` (version 3).  The
trailing result field is optional for those readers: they split on ``;`` and
read the leading fields, so corpora without results keep working.
Everything is resumable: position dumps are appended and the labeler skips
already-labeled FENs when ``--resume`` is given.

Usage:
  python tools/measurement/gen_training_data.py all --positions artifacts/training/positions.txt
      --output artifacts/training/labels.txt --workers 3 --games 20000 --label-depth 8
"""

from __future__ import annotations

import argparse
import os
import queue
import random
import shutil
import sys
import threading
import time
from pathlib import Path

import koi_chess as chess

_VENDORED_STOCKFISH = (
    Path(__file__).resolve().parents[2]
    / "third_party"
    / "stockfish-19"
    / "stockfish-windows-x86-64-universal"
    / "stockfish"
    / "stockfish-windows-x86-64-universal.exe"
)


def _default_stockfish_path() -> Path:
    """The vendored Windows build, or the system ``stockfish`` elsewhere."""
    if os.name == "nt":
        return _VENDORED_STOCKFISH
    found = shutil.which("stockfish")
    return Path(found) if found else _VENDORED_STOCKFISH


DEFAULT_STOCKFISH = _default_stockfish_path()

# Positions with absurd material or book-like shallow plies are skipped.
MIN_PLIES = 6
MAX_PLIES = 140
MAX_ABS_CP = 4000


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def move_weight(board: chess.Board, move: chess.Move) -> int:
    """Weight a noisy playout choice towards captures, checks, and promotions."""
    weight = 1
    if board.is_capture(move):
        weight += 3
    if board.gives_check(move):
        weight += 2
    if move.promotion:
        weight += 4
    return weight


def game_result(board: chess.Board) -> str:
    """White-relative outcome of a finished game as ``1.0``/``0.5``/``0.0``."""
    outcome = board.outcome(claim_draw=True)
    if outcome is None or outcome.winner is None:
        return "0.5"
    return "1.0" if outcome.winner == chess.WHITE else "0.0"


def position_key(board: chess.Board) -> str:
    """Position identity for deduplication (the first four FEN fields)."""
    return " ".join(board.fen().split(" ")[:4])


def load_seen_positions(path: Path) -> set[str]:
    """Seed the position-dedup set from an existing positions file (--resume)."""
    seen: set[str] = set()
    if not path.exists():
        return seen
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            fen = line.split(";", 1)[0]
            if not fen:
                continue
            try:
                seen.add(position_key(chess.Board(fen)))
            except ValueError:
                continue
    return seen


def load_labeled_fens(path: Path) -> set[str]:
    """Read FENs already present in a labels file (--resume)."""
    done: set[str] = set()
    if not path.exists():
        return done
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if parts and parts[0]:
                done.add(parts[0])
    return done


class Labeler:
    """One Stockfish process owned by one worker thread."""

    def __init__(self, engine_path: Path, depth: int, hash_mb: int, threads: int = 1) -> None:
        self.engine = chess.engine.SimpleEngine.popen_uci(str(engine_path))
        self.engine.configure({"Threads": threads, "Hash": hash_mb, "UCI_ShowWDL": False})
        self.depth = depth
        self.limit = chess.engine.Limit(depth=depth)

    def label(self, board: chess.Board) -> tuple[int, str] | None:
        try:
            info = self.engine.analyse(board, self.limit)
        except chess.engine.EngineTerminatedError:
            raise
        score = info["score"].pov(board.turn)
        if score.is_mate():
            return None
        cp = score.score()
        if cp is None or abs(cp) > MAX_ABS_CP:
            return None
        pv = info.get("pv") or []
        best = pv[0].uci() if pv else None
        if best is None:
            return None
        return cp, best

    def best_move(self, board: chess.Board, depth: int) -> chess.Move:
        result = self.engine.play(board, chess.engine.Limit(depth=depth))
        if result.move is None:
            raise chess.engine.EngineError("Stockfish returned no move")
        return result.move

    def close(self) -> None:
        try:
            self.engine.quit()
        except chess.engine.EngineTerminatedError:
            pass


def play_games(args: argparse.Namespace, positions_fh, seen: set[str], counters: dict) -> None:
    rng = random.Random(args.seed + threading.get_ident())
    labeler = Labeler(args.stockfish, depth=args.game_depth, hash_mb=args.game_hash, threads=1)
    try:
        while True:
            with counters["lock"]:
                if counters["games_done"] >= counters["games_target"]:
                    break
                counters["games_done"] += 1
                game_number = counters["games_done"]
            board = chess.Board()
            opening_plies = rng.randint(2, 8)
            for _ in range(opening_plies):
                if board.is_game_over(claim_draw=False):
                    break
                board.push(rng.choice(list(board.legal_moves)))
            positions: list[chess.Board] = []
            plies = opening_plies
            while not board.is_game_over(claim_draw=False) and plies < MAX_PLIES:
                try:
                    move = labeler.best_move(board, args.game_depth)
                except chess.engine.EngineError:
                    break
                board.push(move)
                plies += 1
                if plies >= MIN_PLIES:
                    positions.append(board.copy(stack=True))
            with counters["lock"]:
                result = game_result(board)
                for position in positions:
                    key = position_key(position)
                    if key in seen:
                        continue
                    seen.add(key)
                    positions_fh.write(f"{position.fen()};{result}\n")
                    counters["positions"] += 1
            if game_number % 25 == 0:
                with counters["lock"]:
                    total = counters["positions"]
                    rate = counters["positions"] / max(1e-6, time.time() - counters["start"])
                log(f"games {game_number}/{args.games} positions {total} ({rate:.0f}/s)")
    finally:
        labeler.close()


def noise_games(args: argparse.Namespace, positions_fh, seen: set[str], counters: dict) -> None:
    """Random-ish playouts biased towards captures and checks for tactical coverage."""
    rng = random.Random(args.seed ^ 0x5F3759DF ^ threading.get_ident())
    while True:
        with counters["lock"]:
            if counters["games_done"] >= counters["games_target"]:
                break
            counters["games_done"] += 1
        board = chess.Board()
        plies = 0
        positions: list[chess.Board] = []
        while not board.is_game_over(claim_draw=False) and plies < MAX_PLIES:
            moves = list(board.legal_moves)
            if not moves:
                break
            weighted: list[chess.Move] = []
            for move in moves:
                weighted.extend([move] * move_weight(board, move))
            board.push(rng.choice(weighted))
            plies += 1
            if plies >= MIN_PLIES and not board.is_game_over(claim_draw=False):
                positions.append(board.copy(stack=True))
        with counters["lock"]:
            result = game_result(board)
            for position in positions:
                key = position_key(position)
                if key in seen:
                    continue
                seen.add(key)
                positions_fh.write(f"{position.fen()};{result}\n")
                counters["positions"] += 1
    log("noise game worker finished")


def run_games_stage(args: argparse.Namespace) -> int:
    path = Path(args.positions)
    path.parent.mkdir(parents=True, exist_ok=True)
    seen: set[str] = set()
    if args.resume:
        seen = load_seen_positions(path)
        if seen:
            log(f"resume: seeded {len(seen)} existing positions")
    counters = {
        "games_done": 0,
        "games_target": 0,
        "positions": 0,
        "start": time.time(),
        "lock": threading.Lock(),
    }
    noise_target = int(args.games * args.noise_fraction)
    engine_target = args.games - noise_target
    with open(path, "a", encoding="utf-8") as positions_fh:
        if engine_target > 0:
            counters["games_done"] = 0
            counters["games_target"] = engine_target
            workers = max(1, min(args.workers, engine_target))
            engine_threads = [
                threading.Thread(target=play_games, args=(args, positions_fh, seen, counters), daemon=False)
                for _ in range(workers)
            ]
            for thread in engine_threads:
                thread.start()
            for thread in engine_threads:
                thread.join()
        if noise_target > 0:
            counters["games_done"] = 0
            counters["games_target"] = noise_target
            noise_threads = [
                threading.Thread(target=noise_games, args=(args, positions_fh, seen, counters), daemon=False)
                for _ in range(max(1, min(args.workers, noise_target)))
            ]
            for thread in noise_threads:
                thread.start()
            for thread in noise_threads:
                thread.join()
    log(f"positions written to {path} (total {counters['positions']})")
    return 0


def run_label_stage(args: argparse.Namespace) -> int:
    positions_path = Path(args.positions)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    rows: list[tuple[str, str | None]] = []
    with open(positions_path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            parts = line.split(";", 1)
            if not parts[0]:
                continue
            outcome = parts[1].strip() if len(parts) > 1 else ""
            rows.append((parts[0], outcome or None))
    log(f"loaded {len(rows)} positions")

    done: set[str] = set()
    if args.resume:
        done = load_labeled_fens(output_path)
        if done:
            log(f"resume: {len(done)} positions already labeled")

    work: queue.Queue[tuple[str, str | None] | None] = queue.Queue()
    for fen, outcome in rows:
        if fen in done:
            continue
        work.put((fen, outcome))
    for _ in range(args.workers):
        work.put(None)

    counters = {"labeled": 0, "skipped": 0, "start": time.time(), "lock": threading.Lock()}
    writer_lock = threading.Lock()
    output_fh = open(output_path, "a", encoding="utf-8", buffering=1)
    target = args.limit if args.limit > 0 else len(rows)

    def worker() -> None:
        labeler = Labeler(args.stockfish, depth=args.label_depth, hash_mb=args.label_hash, threads=1)
        try:
            while True:
                item = work.get()
                if item is None:
                    break
                fen, outcome = item
                with counters["lock"]:
                    if counters["labeled"] >= target:
                        work.task_done()
                        continue
                try:
                    board = chess.Board(fen)
                    result = labeler.label(board)
                except (ValueError, chess.engine.EngineTerminatedError):
                    result = None
                if result is None:
                    with counters["lock"]:
                        counters["skipped"] += 1
                else:
                    cp, best = result
                    suffix = f";{outcome}" if outcome else ""
                    with writer_lock:
                        output_fh.write(f"{fen};{cp};{best}{suffix}\n")
                    with counters["lock"]:
                        counters["labeled"] += 1
                        if counters["labeled"] % 5000 == 0:
                            elapsed = time.time() - counters["start"]
                            log(f"labeled {counters['labeled']} ({counters['labeled'] / elapsed:.0f}/s)")
                work.task_done()
        finally:
            labeler.close()

    threads = [threading.Thread(target=worker, daemon=False) for _ in range(args.workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    output_fh.close()
    log(f"labeled {counters['labeled']} positions ({counters['skipped']} skipped); wrote {output_path}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("games", "label", "all"))
    parser.add_argument("--stockfish", type=Path, default=DEFAULT_STOCKFISH)
    parser.add_argument("--positions", type=Path, default=Path("artifacts/training/positions.txt"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/training/labels.txt"))
    parser.add_argument("--games", type=int, default=2000)
    parser.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 4) - 1))
    parser.add_argument("--seed", type=int, default=20260916)
    parser.add_argument("--noise-fraction", type=float, default=0.15)
    parser.add_argument("--game-depth", type=int, default=4)
    parser.add_argument("--game-hash", type=int, default=16)
    parser.add_argument("--label-depth", type=int, default=9)
    parser.add_argument("--label-hash", type=int, default=64)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--resume", action="store_true")
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    if not args.stockfish.exists():
        print(f"Stockfish binary not found: {args.stockfish}", file=sys.stderr)
        return 2
    if args.stage == "games":
        return run_games_stage(args)
    if args.stage == "label":
        return run_label_stage(args)
    code = run_games_stage(args)
    if code != 0:
        return code
    return run_label_stage(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
