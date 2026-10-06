"""Convert Bullet text corpora (``FEN | score | result``) to Koi ``FEN;cp``.

Bullet's text format stores scores from White's perspective
(``tools/nnue/to_bullet.py`` documents the conversion).  Koi's trainer and
engine both treat a corpus score as side-to-move relative, so a
Black-to-move row must have its score negated.  The original v5 official
corpus was built without this flip, which trained the network on
sign-inverted Black-to-move targets and made its evaluations unusable;
``tools/measurement/validate_corpus_perspective.py`` guards against a
repeat.

Use this converter (or the equivalent logic in
``tools/measurement/export_bullet_v5.py``) whenever a Bullet text corpus is
imported for training.
"""

from __future__ import annotations

import argparse
import pathlib

# ``koi_chess`` mirrors the python-chess API subset this tool needs and is
# standard-library only, so the converter (and its regression test) run in CI
# and on machines without python-chess installed.
import koi_chess as chess


def convert_line(line: str) -> str | None:
    """Convert one ``FEN | score | ...`` row to ``FEN;stm_cp``.

    Returns ``None`` for blank or malformed rows.
    """
    line = line.strip()
    if not line:
        return None
    parts = line.split(" | ")
    if len(parts) < 2:
        return None
    board = chess.Board(parts[0])
    white_cp = int(float(parts[1]))
    score = white_cp if board.turn == chess.WHITE else -white_cp
    return f"{parts[0]};{score}"


def convert_file(
    source: pathlib.Path,
    destination: pathlib.Path,
    progress_every: int = 200000,
) -> int:
    """Convert ``source`` into ``destination`` and return the rows written."""
    written = 0
    with source.open("r", encoding="utf-8", errors="replace") as handle, destination.open(
        "w", encoding="utf-8", newline="\n"
    ) as output:
        for line in handle:
            converted = convert_line(line)
            if converted is None:
                continue
            output.write(converted + "\n")
            written += 1
            if progress_every and written % progress_every == 0:
                print(f"{destination.name}: {written} rows", flush=True)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    written = convert_file(args.input, args.output)
    print(f"{args.input} -> {args.output}: {written} rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
