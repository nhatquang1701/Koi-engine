"""Build a Polyglot opening book from Lichess's open-source opening data.

Source: https://github.com/lichess-org/chess-openings (CC0 1.0 Universal).  The
data is one TSV per first letter with `eco`, `name`, and `pgn` columns; every
row is a named line.  Each (position, move) pair that appears in a named line
becomes a book entry, and its weight is the number of named lines that play it,
so the most-established continuations are preferred.

The output is a standard Polyglot `.bin` (16-byte big-endian entries sorted by
key), which is what the engine reads through `state.polyglot_key()`.

Usage:
    python3 tools/book/build_book.py --source openings/ --output book.bin
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import struct
import sys

import chess
import chess.polyglot

MOVE_NUMBER = re.compile(r"\d+\.(?:\.\.)?")
MAX_WEIGHT = 0xFFFF
PROMOTION_CODES = {
    chess.KNIGHT: 1,
    chess.BISHOP: 2,
    chess.ROOK: 3,
    chess.QUEEN: 4,
}


def parse_line(text: str) -> list[chess.Move]:
    """Turn the `pgn` column (`1. Nh3 d5 2. g3 e5 3. f4`) into moves."""
    board = chess.Board()
    moves: list[chess.Move] = []
    cleaned = MOVE_NUMBER.sub(" ", text)
    for token in cleaned.split():
        token = token.strip()
        if not token or token in {"1-0", "0-1", "1/2-1/2", "*"}:
            continue
        try:
            move = board.parse_san(token)
        except ValueError:
            break
        moves.append(move)
        board.push(move)
    return moves


def load_rows(source: pathlib.Path) -> list[list[chess.Move]]:
    rows: list[list[chess.Move]] = []
    for path in sorted(source.glob("*.tsv")):
        with path.open(encoding="utf-8") as handle:
            header = handle.readline().rstrip("\n").split("\t")
            if "pgn" not in header:
                print(f"skipping {path.name}: no pgn column", file=sys.stderr)
                continue
            pgn_column = header.index("pgn")
            for raw in handle:
                fields = raw.rstrip("\n").split("\t")
                if len(fields) <= pgn_column:
                    continue
                moves = parse_line(fields[pgn_column])
                if moves:
                    rows.append(moves)
    return rows


def build_entries(rows: list[list[chess.Move]], max_ply: int) -> dict[int, dict[chess.Move, int]]:
    book: dict[int, dict[chess.Move, int]] = {}
    for moves in rows:
        board = chess.Board()
        for ply, move in enumerate(moves):
            if ply >= max_ply:
                break
            key = chess.polyglot.zobrist_hash(board)
            entry = book.setdefault(key, {})
            entry[move] = entry.get(move, 0) + 1
            board.push(move)
    return book


def encode(book: dict[int, dict[chess.Move, int]]) -> bytes:
    entries: list[tuple[int, int, int]] = []
    for key, moves in book.items():
        for move, weight in moves.items():
            encoded = ((move.to_square & 0x3F)
                       | ((move.from_square & 0x3F) << 6)
                       | (PROMOTION_CODES.get(move.promotion, 0) << 12))
            entries.append((key, encoded, min(weight, MAX_WEIGHT)))
    entries.sort(key=lambda item: (item[0], -item[2], item[1]))
    payload = bytearray()
    for key, move, weight in entries:
        payload += struct.pack(">QHHI", key, move, weight, 0)
    return bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a Polyglot book from lichess openings")
    parser.add_argument("--source", required=True, help="directory holding the lichess *.tsv files")
    parser.add_argument("--output", required=True, help="path of the .bin to write")
    parser.add_argument("--max-ply", type=int, default=24, help="deepest ply to index (default 24)")
    args = parser.parse_args()

    rows = load_rows(pathlib.Path(args.source).expanduser())
    if not rows:
        print("no opening lines found", file=sys.stderr)
        return 1
    book = build_entries(rows, args.max_ply)
    payload = encode(book)

    output = pathlib.Path(args.output).expanduser()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(payload)

    entries = sum(len(entry) for entry in book.values())
    print(f"lines {len(rows)}  positions {len(book)}  entries {entries}  "
          f"bytes {len(payload)}  sha256 {hashlib.sha256(payload).hexdigest()}")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
