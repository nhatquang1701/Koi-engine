"""Validate that a ``FEN;cp`` corpus uses side-to-move-relative scores.

The trainer pairs raw ``cp`` targets with side-to-move-oriented feature blocks,
and the engine reads the network output as side-to-move relative.  A corpus
whose scores are white-relative (or otherwise perspective-inconsistent) trains a
sign-broken network, so refuse to proceed unless the sampled correlation between
the score and the side-to-move material balance is strong.

Usage:
    python tools/measurement/validate_corpus_perspective.py <corpus.txt> [--sample 20000] [--threshold 0.8]
"""

from __future__ import annotations

import argparse
import pathlib
import random
import sys

import chess

PIECE_CP = {
    chess.PAWN: 100,
    chess.KNIGHT: 320,
    chess.BISHOP: 330,
    chess.ROOK: 500,
    chess.QUEEN: 900,
}


def material_stm(board: chess.Board) -> int:
    balance = 0
    for piece_type, value in PIECE_CP.items():
        balance += value * len(board.pieces(piece_type, chess.WHITE))
        balance -= value * len(board.pieces(piece_type, chess.BLACK))
    return balance if board.turn == chess.WHITE else -balance


def pearson(xs: list[float], ys: list[float]) -> float:
    n = len(xs)
    mx = sum(xs) / n
    my = sum(ys) / n
    cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    vx = sum((x - mx) ** 2 for x in xs)
    vy = sum((y - my) ** 2 for y in ys)
    if vx == 0 or vy == 0:
        return 0.0
    return cov / (vx**0.5 * vy**0.5)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("corpus", type=pathlib.Path)
    parser.add_argument("--sample", type=int, default=20000)
    parser.add_argument("--threshold", type=float, default=0.8)
    parser.add_argument("--seed", type=int, default=20261006)
    args = parser.parse_args()

    if not args.corpus.exists():
        print(f"corpus not found: {args.corpus}", file=sys.stderr)
        return 2

    reservoir: list[tuple[chess.Board, int]] = []
    seen = 0
    with args.corpus.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            parts = raw.split(";")
            if len(parts) < 2:
                continue
            try:
                board = chess.Board(parts[0])
                score = int(parts[1])
            except ValueError:
                continue
            seen += 1
            if len(reservoir) < args.sample:
                reservoir.append((board, score))
            else:
                j = random.Random(args.seed + seen).randrange(seen)
                if j < args.sample:
                    reservoir[j] = (board, score)

    if not reservoir:
        print("no usable rows", file=sys.stderr)
        return 3

    xs = [float(material_stm(board)) for board, _ in reservoir]
    ys = [float(score) for _, score in reservoir]
    r = pearson(xs, ys)
    print(f"rows {seen} sampled {len(reservoir)}")
    print(f"pearson(stm_material, cp) {r:.4f}")
    if r < args.threshold:
        print(
            f"FAIL: side-to-move correlation {r:.4f} < {args.threshold} - the corpus "
            "scores do not look side-to-move relative; do not train on it",
            file=sys.stderr,
        )
        return 1
    print("PASS: corpus scores look side-to-move relative")
    return 0


if __name__ == "__main__":
    sys.exit(main())
