"""Preselect color-reversed strength openings before the release matches.

Each draw independently selects one of the curated roots uniformly, then one
legal one-ply continuation uniformly. Sampling is with replacement: duplicate
positions still represent separate draws from the declared opening distribution.
The fixed seed and committed output make the sample auditable before results.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "measurement"))
import koi_chess as chess  # noqa: E402


def sample_openings(
    curated: Path, *, sample_count: int, seed: int
) -> list[tuple[str, tuple[str, ...]]]:
    if sample_count < 1:
        raise ValueError("sample_count must be positive")
    rng = random.Random(seed)
    roots: list[tuple[str, tuple[str, ...], tuple[str, ...]]] = []
    names: set[str] = set()
    for number, raw in enumerate(curated.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.count("|") != 1:
            raise ValueError(f"curated line {number} must contain one separator")
        name, move_list = (part.strip() for part in line.split("|"))
        if not name or name in names:
            raise ValueError(f"curated line {number} has a missing or duplicate name")
        names.add(name)
        root_moves = tuple(move_list.split())
        board = chess.Board()
        for text in root_moves:
            move = chess.Move.from_uci(text)
            if not board.is_legal(move):
                raise ValueError(f"curated line {number} contains illegal move {text}")
            board.push(move)
        legal = tuple(sorted(move.uci() for move in board.legal_moves))
        if not legal:
            raise ValueError(f"curated line {number} has no legal continuation")
        roots.append((name, root_moves, legal))
    if not roots:
        raise ValueError("curated opening file has no entries")
    openings: list[tuple[str, tuple[str, ...]]] = []
    for sample_number in range(1, sample_count + 1):
        name, root_moves, legal = rng.choice(roots)
        continuation = rng.choice(legal)
        openings.append(
            (f"{name}-sample-{sample_number}", root_moves + (continuation,))
        )
    return openings


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--curated", required=True, type=Path)
    parser.add_argument("--sample-count", required=True, type=int)
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    sample = sample_openings(
        args.curated, sample_count=args.sample_count, seed=args.seed
    )
    header = (
        f"# Independent uniform curated-root and legal one-ply continuation draws, "
        f"with replacement; seed={args.seed}; sample_count={args.sample_count}\n"
    )
    lines = [f"{name} | {' '.join(moves)}" for name, moves in sample]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header + "\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(sample)} sampled openings to {args.output}")


if __name__ == "__main__":
    main()
