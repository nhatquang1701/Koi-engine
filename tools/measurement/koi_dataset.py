"""Binary training corpus encoder for Koi NNUE (``koi-dataset-v2``).

Converts the text corpus written by ``gen_training_data.py``
(``FEN;cp;best_move`` rows) into the sparse binary dataset consumed by the
v5 trainers.  Rows whose FEN is invalid, whose score is missing, or whose
score exceeds the corpus score limit are skipped, matching the label filters.

Format (little-endian):

    magic "KOI-DATA" (8 bytes)
    u32 version (2)
    u16 group count (2)
    per group: u16 feature-set length, feature-set string
        (group A ``halfka-king-bucket-v1``, group B ``threat-pairs-v1``)
    u64 position count
    per record:
        u16 counts[4]        (A side to move, B side to move, A opponent,
                              B opponent)
        u16 indices[...]     (strictly increasing inside each block)
        i32 score_cp         (side-to-move relative)

Both perspectives are stored because the opponent's perspective-normalized
features cannot be derived from the side-to-move list alone.  The version 1
reader is retained for ``info`` and regression tests.

Writing is streaming and resumable.  A checkpoint state file
(``<output>.state.json``) records the input byte offset, the record count, and
the number of record bytes written; ``--resume`` seeks back to that offset and
continues appending.  ``--limit N`` stops after N new records and keeps the
checkpoint, which makes long corpus encodes restartable; a run that reaches
the end of the input writes the final record count into the header and removes
the state file.

Usage:
  python tools/measurement/koi_dataset.py encode `
      --input artifacts/training/labels.txt --output artifacts/training/koi-dataset-v2.bin
  python tools/measurement/koi_dataset.py info --input artifacts/training/koi-dataset-v2.bin
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
THREAT_FEATURE_SET = "threat-pairs-v1"
GROUP_FEATURE_SETS = (FEATURE_SET, THREAT_FEATURE_SET)
COMBINED_FEATURE_SET = "halfka-king-bucket-v1+threat-pairs-v1"
MAGIC = b"KOI-DATA"
VERSION = 2
STATE_SCHEMA = "koi-dataset-v2-state"
INPUT_UNITS = 12 * 12 * 64
THREAT_BUCKET_UNITS = 2304
THREAT_INPUT_UNITS = 12 * THREAT_BUCKET_UNITS
TOTAL_INPUT_UNITS = INPUT_UNITS + THREAT_INPUT_UNITS
SPARSE_CAPACITY = 64
THREAT_SPARSE_CAPACITY = 128
V5_SPARSE_CAPACITY = 160
MAX_ABS_CP = 4000
FLUSH_RECORDS = 4096
FEATURE_SET_BYTES = FEATURE_SET.encode("utf-8")
THREAT_FEATURE_SET_BYTES = THREAT_FEATURE_SET.encode("utf-8")


def header_bytes() -> int:
    payload = 8 + 4 + 2 + 8
    for name in (FEATURE_SET_BYTES, THREAT_FEATURE_SET_BYTES):
        payload += 2 + len(name)
    return payload


HEADER_BYTES = header_bytes()


class DatasetError(Exception):
    """A user-facing encoder error."""


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def perspective_square(square: int, perspective: chess.Color) -> int:
    """Square in the given real-color perspective (black mirrors vertically)."""
    return square if perspective == chess.WHITE else square ^ 56


def king_bucket_for(board: chess.Board, perspective: chess.Color) -> int:
    """Own-king bucket 0..11 for a real-color perspective (0 when absent)."""
    king_square = board.king(perspective)
    if king_square is None:
        return 0
    square = perspective_square(king_square, perspective)
    file = square & 7
    rank = square >> 3
    mirrored_file = file + 4 if file < 4 else file
    if rank <= 2:
        zone = 0
    elif rank <= 5:
        zone = 1
    else:
        zone = 2
    return zone * 4 + (mirrored_file - 4)


def king_bucket(board: chess.Board) -> int:
    """Own-king bucket 0..11 in the side-to-move perspective (0 when absent)."""
    return king_bucket_for(board, board.turn)


def halfka_king_bucket_indices_for(
    board: chess.Board, perspective: chess.Color
) -> list[int]:
    """Active sparse indices of ``halfka-king-bucket-v1`` in one perspective."""
    mirror = perspective == chess.BLACK
    base = king_bucket_for(board, perspective) * 768
    indices: list[int] = []
    for square, piece in board.piece_map().items():
        own = piece.color == perspective
        plane = piece.piece_type - 1 if own else piece.piece_type + 5
        indices.append(base + plane * 64 + (square ^ 56 if mirror else square))
    indices.sort()
    return indices


def halfka_king_bucket_indices(board: chess.Board) -> list[int]:
    """Active sparse indices of ``halfka-king-bucket-v1`` for one position."""
    return halfka_king_bucket_indices_for(board, board.turn)


def attacks_mask(board: chess.Board, square: int) -> int:
    """Pseudo-legal attack bitboard of the piece on ``square``."""
    try:
        return board.attacks_mask(square)
    except AttributeError:  # pragma: no cover - older python-chess
        mask = 0
        for target in board.attacks(square):
            mask |= 1 << target
        return mask


def threat_pairs_indices(
    board: chess.Board, perspective: chess.Color | None = None
) -> list[int]:
    """Sorted, deduplicated ``threat-pairs-v1`` indices for one perspective.

    Every attack relation in the position (either colour attacking the other)
    contributes one input, expressed in the requested perspective's
    coordinates, so both perspectives see the same relations.  Non-slider
    families index the victim type and perspective victim square; sliders
    additionally index the perspective attacker square.
    """
    if perspective is None:
        perspective = board.turn
    if not board.occupied_co[chess.WHITE] or not board.occupied_co[chess.BLACK]:
        return []
    base = INPUT_UNITS + king_bucket_for(board, perspective) * THREAT_BUCKET_UNITS
    indices: set[int] = set()
    for square, piece in board.piece_map().items():
        victims = attacks_mask(board, square) & board.occupied_co[not piece.color]
        if not victims:
            continue
        attacker_square = perspective_square(square, perspective)
        while victims:
            lsb = victims & -victims
            victim_square = lsb.bit_length() - 1
            victims ^= lsb
            victim = board.piece_at(victim_square)
            victim_type = victim.piece_type - 1
            victim_perspective = perspective_square(victim_square, perspective)
            if piece.piece_type == chess.PAWN:
                offset = victim_type * 64 + victim_perspective
            elif piece.piece_type == chess.KNIGHT:
                offset = 384 + victim_type * 64 + victim_perspective
            elif piece.piece_type == chess.KING:
                offset = 1920 + victim_type * 64 + victim_perspective
            else:
                slider_index = {
                    chess.BISHOP: 0,
                    chess.ROOK: 1,
                    chess.QUEEN: 2,
                }[piece.piece_type]
                offset = 768 + slider_index * 384 + attacker_square * 6 + victim_type
            indices.add(base + offset)
    return sorted(indices)


def halfka_threat_v5_indices(
    board: chess.Board, perspective: chess.Color | None = None
) -> list[int]:
    """Sorted combined v5 indices (group A then group B) for one perspective."""
    if perspective is None:
        perspective = board.turn
    merged = set(halfka_king_bucket_indices_for(board, perspective))
    merged.update(threat_pairs_indices(board, perspective))
    return sorted(merged)[:V5_SPARSE_CAPACITY]


def encode_record(board: chess.Board) -> tuple[list[int], list[int], list[int], list[int]]:
    """The four index blocks for one record: A/B for side to move and opponent."""
    stm = board.turn
    opponent = not stm
    return (
        halfka_king_bucket_indices_for(board, stm),
        threat_pairs_indices(board, stm),
        halfka_king_bucket_indices_for(board, opponent),
        threat_pairs_indices(board, opponent),
    )


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
    """Streaming, resumable writer for ``koi-dataset-v2``."""

    def __init__(self, output: Path, input_path: Path) -> None:
        self.output = output
        self.input_path = input_path
        self.state_path = output.parent / (output.name + ".state.json")
        self.count_offset = HEADER_BYTES - 8
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
            if (
                state.get("input") != str(self.input_path)
                or list(state.get("groups", ())) != list(GROUP_FEATURE_SETS)
            ):
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
            self.handle.write(struct.pack("<8sIH", MAGIC, VERSION, len(GROUP_FEATURE_SETS)))
            for name in GROUP_FEATURE_SETS:
                encoded = name.encode("utf-8")
                self.handle.write(struct.pack("<H", len(encoded)))
                self.handle.write(encoded)
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
    def add(self, groups: tuple[list[int], list[int], list[int], list[int]], cp: int) -> None:
        counts = [len(block) for block in groups]
        if counts[0] > SPARSE_CAPACITY or counts[2] > SPARSE_CAPACITY:
            raise DatasetError(
                f"group A record has {max(counts[0], counts[2])} active features; "
                f"capacity is {SPARSE_CAPACITY}"
            )
        if counts[1] > THREAT_SPARSE_CAPACITY or counts[3] > THREAT_SPARSE_CAPACITY:
            raise DatasetError(
                f"group B record has {max(counts[1], counts[3])} active features; "
                f"capacity is {THREAT_SPARSE_CAPACITY}"
            )
        record = struct.pack("<4H", *counts)
        for block in groups:
            record += struct.pack(f"<{len(block)}H", *block)
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
                self.add(encode_record(board), cp)
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
            "groups": list(GROUP_FEATURE_SETS),
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
        magic, version = struct.unpack_from("<8sI", data, 0)
        if version == 1:
            feature_length = struct.unpack_from("<H", data, 12)[0]
            feature_set = data[14 : 14 + feature_length].decode("utf-8", errors="replace")
            count = struct.unpack_from("<Q", data, 14 + feature_length)[0]
            return {
                "magic": magic.decode("ascii", errors="replace"),
                "version": version,
                "feature_set": feature_set,
                "records": count,
            }
        if version == 2:
            group_count = struct.unpack_from("<H", data, 12)[0]
            offset = 14
            groups = []
            for _ in range(group_count):
                length = struct.unpack_from("<H", data, offset)[0]
                offset += 2
                groups.append(data[offset : offset + length].decode("utf-8", errors="replace"))
                offset += length
            count = struct.unpack_from("<Q", data, offset)[0]
            return {
                "magic": magic.decode("ascii", errors="replace"),
                "version": version,
                "groups": groups,
                "records": count,
            }
        raise struct.error(f"unsupported dataset version {version}")

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
