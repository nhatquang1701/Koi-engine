"""Binary training corpus encoder for Koi NNUE (``koi-dataset-v1``).

Converts the text corpus written by ``gen_training_data.py``
(``FEN;cp;best_move`` rows) into the sparse binary dataset consumed by the
``halfka-king-bucket-v1`` trainer.  Rows whose FEN is invalid, whose score is
missing, or whose score exceeds the corpus score limit are skipped, matching
the label filters.

Format (little-endian):

    magic "KOI-DATA" (8 bytes)
    u32 version (1)
    u16 feature-set length
    feature-set string
    u64 position count
    per record:
        u16 count
        u16 indices[count]   (strictly increasing)
        i32 score_cp

Writing is streaming and resumable.  A checkpoint state file
(``<output>.state.json``) records the input byte offset, the record count, and
the number of record bytes written; ``--resume`` seeks back to that offset and
continues appending.  ``--limit N`` stops after N new records and keeps the
checkpoint, which makes long corpus encodes restartable; a run that reaches
the end of the input writes the final record count into the header and removes
the state file.

Usage:
  python tools/measurement/koi_dataset.py encode `
      --input artifacts/training/labels.txt --output artifacts/training/koi-dataset-v1.bin
  python tools/measurement/koi_dataset.py info --input artifacts/training/koi-dataset-v1.bin
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from pathlib import Path

import chess

FEATURE_SET = "halfka-king-bucket-v1"
MAGIC = b"KOI-DATA"
VERSION = 1
STATE_SCHEMA = "koi-dataset-v1-state"
INPUT_UNITS = 12 * 12 * 64
SPARSE_CAPACITY = 64
MAX_ABS_CP = 4000
FLUSH_RECORDS = 4096
FEATURE_SET_BYTES = FEATURE_SET.encode("utf-8")
HEADER_BYTES = 8 + 4 + 2 + len(FEATURE_SET_BYTES) + 8


class DatasetError(Exception):
    """A user-facing encoder error."""


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def king_bucket(board: chess.Board) -> int:
    """Own-king bucket 0..11 in the side-to-move perspective (0 when absent)."""
    king_square = board.king(board.turn)
    if king_square is None:
        return 0
    if board.turn == chess.BLACK:
        king_square ^= 56
    file = king_square & 7
    rank = king_square >> 3
    mirrored_file = file + 4 if file < 4 else file
    if rank <= 2:
        zone = 0
    elif rank <= 5:
        zone = 1
    else:
        zone = 2
    return zone * 4 + (mirrored_file - 4)


def halfka_king_bucket_indices(board: chess.Board) -> list[int]:
    """Active sparse indices of ``halfka-king-bucket-v1`` for one position."""
    turn = board.turn
    mirror = turn == chess.BLACK
    base = king_bucket(board) * 768
    indices: list[int] = []
    for square, piece in board.piece_map().items():
        own = piece.color == turn
        plane = piece.piece_type - 1 if own else piece.piece_type + 5
        perspective_square = (square ^ 56) if mirror else square
        indices.append(base + plane * 64 + perspective_square)
    indices.sort()
    return indices


def output_bucket(board: chess.Board) -> int:
    """Piece-count output bucket 0..7 (0 also covers more than 32 pieces)."""
    pieces = len(board.piece_map())
    if pieces > 32:
        return 0
    return min(7, (32 - pieces) // 4)


def parse_row(raw: bytes) -> tuple[chess.Board, int] | None:
    """Parse one ``FEN;cp;best_move`` line; return None when the row is skipped."""
    line = raw.decode("utf-8", errors="replace").strip()
    if not line:
        return None
    parts = line.split(";")
    if len(parts) < 2:
        return None
    try:
        cp = int(parts[1])
    except ValueError:
        return None
    if abs(cp) > MAX_ABS_CP:
        return None
    try:
        board = chess.Board(parts[0])
    except ValueError:
        return None
    return board, cp


class DatasetEncoder:
    """Streaming, resumable writer for ``koi-dataset-v1``."""

    def __init__(self, output: Path, input_path: Path) -> None:
        self.output = output
        self.input_path = input_path
        self.state_path = output.parent / (output.name + ".state.json")
        self.count_offset = 14 + len(FEATURE_SET_BYTES)
        self.data_start = HEADER_BYTES
        self.offset = 0
        self.records = 0
        self.data_bytes = 0
        self.handle = None
        self._pending: list[bytes] = []

    # -- lifecycle ---------------------------------------------------------
    def start(self, resume: bool) -> bool:
        """Open the output. Returns True when the dataset is already complete."""
        if self.output.exists():
            if not resume:
                raise DatasetError(f"{self.output} already exists; pass --resume to continue it")
            if not self.state_path.exists():
                log(f"{self.output} is already complete; nothing to do")
                return True
        if self.output.exists() and self.state_path.exists():
            state = json.loads(self.state_path.read_text(encoding="utf-8"))
            if state.get("schema") != STATE_SCHEMA:
                raise DatasetError(f"{self.state_path} has an unknown schema")
            if state.get("input") != str(self.input_path) or state.get("feature_set") != FEATURE_SET:
                raise DatasetError(
                    f"{self.state_path} belongs to a different input; delete it to start over"
                )
            expected = self.data_start + int(state["data_bytes"])
            actual = self.output.stat().st_size
            if actual != expected:
                raise DatasetError(
                    f"partial dataset is {actual} bytes but the checkpoint expects {expected}; "
                    "delete it to start over"
                )
            self.offset = int(state["offset"])
            self.records = int(state["records"])
            self.data_bytes = int(state["data_bytes"])
            self.handle = open(self.output, "r+b")
            self.handle.seek(0, os.SEEK_END)
            log(f"resume: {self.records} records checkpointed at input offset {self.offset}")
        else:
            if self.state_path.exists():
                log(f"ignoring stale checkpoint {self.state_path}")
                self.state_path.unlink()
            self.output.parent.mkdir(parents=True, exist_ok=True)
            self.handle = open(self.output, "w+b")
            self.handle.write(struct.pack("<8sIH", MAGIC, VERSION, len(FEATURE_SET_BYTES)))
            self.handle.write(FEATURE_SET_BYTES)
            self.handle.write(struct.pack("<Q", 0))
            self.handle.flush()
            log(f"created {self.output}")
        self._save_state()
        return False

    def close(self) -> None:
        if self.handle is not None:
            try:
                self.handle.close()
            finally:
                self.handle = None

    # -- writing -----------------------------------------------------------
    def add(self, indices: list[int], cp: int) -> None:
        if len(indices) > SPARSE_CAPACITY:
            raise DatasetError(f"record has {len(indices)} active features; capacity is {SPARSE_CAPACITY}")
        record = struct.pack(f"<H{len(indices)}H", len(indices), *indices)
        record += struct.pack("<i", cp)
        self._pending.append(record)
        self.records += 1
        self.data_bytes += len(record)
        if len(self._pending) >= FLUSH_RECORDS:
            self._flush()

    def run(self, limit: int, progress_every: int) -> int:
        if not self.input_path.exists():
            raise DatasetError(f"input corpus not found: {self.input_path}")
        processed = 0
        with open(self.input_path, "rb") as source:
            source.seek(self.offset)
            while True:
                if limit and processed >= limit:
                    self._flush()
                    log(
                        f"checkpointed {self.records} records at input offset {self.offset}; "
                        "rerun with --resume to continue"
                    )
                    return 0
                raw = source.readline()
                if not raw:
                    break
                self.offset = source.tell()
                parsed = parse_row(raw)
                if parsed is None:
                    continue
                board, cp = parsed
                self.add(halfka_king_bucket_indices(board), cp)
                processed += 1
                if progress_every and self.records % progress_every == 0:
                    self._flush()
                    log(f"encoded {self.records} records")
        self._finalize()
        return 0

    def _flush(self) -> None:
        if self._pending:
            self.handle.write(b"".join(self._pending))
            self._pending.clear()
        self.handle.flush()
        os.fsync(self.handle.fileno())
        self._save_state()

    def _save_state(self) -> None:
        state = {
            "schema": STATE_SCHEMA,
            "input": str(self.input_path),
            "feature_set": FEATURE_SET,
            "offset": self.offset,
            "records": self.records,
            "data_bytes": self.data_bytes,
        }
        self.state_path.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")

    def _finalize(self) -> None:
        self._flush()
        self.handle.seek(self.count_offset)
        self.handle.write(struct.pack("<Q", self.records))
        self.handle.flush()
        os.fsync(self.handle.fileno())
        self.close()
        self.state_path.unlink(missing_ok=True)
        log(f"wrote {self.output} ({self.records} records, {self.output.stat().st_size} bytes)")

    # -- inspection --------------------------------------------------------
    @staticmethod
    def read_header_bytes(data: bytes) -> dict:
        magic, version, feature_length = struct.unpack_from("<8sIH", data, 0)
        feature_set = data[14 : 14 + feature_length].decode("utf-8", errors="replace")
        count = struct.unpack_from("<Q", data, 14 + feature_length)[0]
        return {
            "magic": magic.decode("ascii", errors="replace"),
            "version": version,
            "feature_set": feature_set,
            "records": count,
        }

    @staticmethod
    def read_header(path: Path) -> dict:
        with open(path, "rb") as handle:
            return DatasetEncoder.read_header_bytes(handle.read())


def encode_command(args: argparse.Namespace) -> int:
    encoder = DatasetEncoder(args.output, args.input)
    try:
        if encoder.start(args.resume):
            return 0
        return encoder.run(args.limit, args.progress_every)
    except DatasetError as error:
        print(f"koi_dataset.py: {error}", file=sys.stderr)
        return 2
    finally:
        encoder.close()


def info_command(path: Path) -> int:
    if not path.exists():
        print(f"koi_dataset.py: dataset not found: {path}", file=sys.stderr)
        return 2
    try:
        header = DatasetEncoder.read_header(path)
    except (OSError, struct.error) as error:
        print(f"koi_dataset.py: {error}", file=sys.stderr)
        return 2
    state_path = path.parent / (path.name + ".state.json")
    header.update(
        {
            "path": str(path),
            "bytes": path.stat().st_size,
            "complete": not state_path.exists(),
        }
    )
    print(json.dumps(header, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    encode = subparsers.add_parser("encode", help="encode a FEN;cp;best_move corpus")
    encode.add_argument("--input", type=Path, required=True)
    encode.add_argument("--output", type=Path, required=True)
    encode.add_argument("--resume", action="store_true", help="continue a checkpointed dataset")
    encode.add_argument(
        "--limit",
        type=int,
        default=0,
        help="stop after N new records and keep the checkpoint (0 encodes everything)",
    )
    encode.add_argument("--progress-every", type=int, default=100000)
    info = subparsers.add_parser("info", help="print the header of an encoded dataset")
    info.add_argument("--input", type=Path, required=True)
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    if args.command == "info":
        return info_command(args.input)
    return encode_command(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
