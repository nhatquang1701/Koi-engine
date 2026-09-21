"""Convert Koi label corpora into bulletformat datasets.

Reads the ``FEN;cp;best_move[;result]`` label corpus (side-to-move
centipawns), writes white-relative bullet text rows
``<FEN> | <score> | <result>``, and converts them to ``bulletformat``
``ChessBoard`` binaries with the ``convert`` binary from
``tools/nnue/bullet_train``.

The game result is carried through when the corpus records one (the games
stage writes it as a white-relative ``1.0``/``0.5``/``0.0``); otherwise the
pseudo-result ``0.5`` is written.  How much the result matters is decided by
the trainer's ``--wdl`` weight, which defaults to ``0.5``.

Usage:
    python tools/nnue/to_bullet.py --input artifacts/training/labels.txt \\
        --output-dir artifacts/training/bullet
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MEASUREMENT_DIR = REPO_ROOT / "tools" / "measurement"
if str(MEASUREMENT_DIR) not in sys.path:
    sys.path.insert(0, str(MEASUREMENT_DIR))

import koi_dataset  # noqa: E402  (path setup above)

DEFAULT_INPUT = REPO_ROOT / "artifacts" / "training" / "labels.txt"
DEFAULT_OUTPUT_DIR = REPO_ROOT / "artifacts" / "training" / "bullet"
DEFAULT_CONVERT_CANDIDATES = (
    REPO_ROOT / "tools" / "nnue" / "bullet_train" / "target" / "release" / "convert.exe",
    REPO_ROOT / "tools" / "nnue" / "bullet_train" / "target" / "debug" / "convert.exe",
)
PSEUDO_RESULT = "0.5"
RESULT_TOKENS = frozenset({"1.0", "0.5", "0.0"})


class ConversionError(RuntimeError):
    """Raised when the corpus cannot be converted."""


def bullet_text_row(fen: str, side_to_move_white: bool, cp: int, result: str = PSEUDO_RESULT) -> str:
    """Build one white-relative bullet text row."""
    white_cp = cp if side_to_move_white else -cp
    return f"{fen} | {white_cp} | {result}"


def row_result(raw: str) -> str:
    """Return the white-relative result recorded on a corpus row, if any."""
    parts = raw.strip().split(";")
    if len(parts) > 3 and parts[3] in RESULT_TOKENS:
        return parts[3]
    return PSEUDO_RESULT


def validation_stride(val_fraction: float) -> int:
    """Return the row stride that yields approximately ``val_fraction`` validation rows."""
    if val_fraction <= 0.0:
        return 0
    if val_fraction >= 1.0:
        return 1
    return max(2, int(round(1.0 / val_fraction)))


def convert_labels(
    input_path: Path,
    output_dir: Path,
    val_fraction: float = 0.05,
    limit: int = 0,
    log=print,
) -> tuple[int, int]:
    """Write train/validation bullet text files and return their row counts."""
    if not input_path.is_file():
        raise ConversionError(f"input corpus not found: {input_path}")
    output_dir.mkdir(parents=True, exist_ok=True)

    stride = validation_stride(val_fraction)
    train_rows: list[str] = []
    validation_rows: list[str] = []
    accepted = 0
    skipped = 0
    recorded = 0

    with input_path.open("r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            if limit and accepted >= limit:
                break
            parsed = koi_dataset.parse_row(raw.encode("utf-8"))
            if parsed is None:
                skipped += 1
                continue
            board, cp = parsed
            result = row_result(raw)
            if result != PSEUDO_RESULT:
                recorded += 1
            index = accepted
            accepted += 1
            row = bullet_text_row(board.fen(), board.turn, cp, result)
            if stride and index % stride == 0:
                validation_rows.append(row)
            else:
                train_rows.append(row)

    if accepted == 0:
        raise ConversionError(f"no usable rows in {input_path}")

    train_path = output_dir / "train.txt"
    train_path.write_text("\n".join(train_rows) + "\n", encoding="utf-8")
    validation_path = output_dir / "validation.txt"
    if validation_rows:
        validation_path.write_text("\n".join(validation_rows) + "\n", encoding="utf-8")
    elif validation_path.exists():
        validation_path.unlink()

    log(f"to_bullet: wrote {len(train_rows)} train rows and {len(validation_rows)} validation rows")
    if recorded:
        log(f"to_bullet: carried a game result for {recorded} rows")
    if skipped:
        log(f"to_bullet: skipped {skipped} unusable lines")
    return len(train_rows), len(validation_rows)


def find_convert_exe(explicit: Path | None = None) -> Path | None:
    """Locate the bullet_train ``convert`` binary."""
    if explicit is not None:
        return explicit if explicit.is_file() else None
    for candidate in DEFAULT_CONVERT_CANDIDATES:
        if candidate.is_file():
            return candidate
    return None


def run_converter(convert_exe: Path, text_path: Path, data_path: Path, log=print) -> None:
    """Convert one bullet text file to a ``.data`` binary."""
    result = subprocess.run(
        [str(convert_exe), str(text_path), str(data_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise ConversionError(f"convert failed for {text_path.name}: {detail}")
    log(f"to_bullet: converted {data_path} ({data_path.stat().st_size} bytes)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Convert Koi labels into bulletformat datasets.")
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT, help="label corpus (FEN;cp;best_move)")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="where train/validation files go")
    parser.add_argument("--val-fraction", type=float, default=0.05, help="validation row fraction (0 disables)")
    parser.add_argument("--limit", type=int, default=0, help="cap on accepted rows (0 = all)")
    parser.add_argument("--convert-exe", type=Path, default=None, help="path to the bullet_train convert binary")
    parser.add_argument("--text-only", action="store_true", help="write the bullet text files without converting")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        train_count, validation_count = convert_labels(
            args.input, args.output_dir, args.val_fraction, args.limit
        )
        if args.text_only:
            return 0
        convert_exe = find_convert_exe(args.convert_exe)
        if convert_exe is None:
            raise ConversionError(
                "convert binary not found; build it with "
                "'cargo build --release --bin convert' in tools/nnue/bullet_train "
                "or pass --convert-exe"
            )
        run_converter(convert_exe, args.output_dir / "train.txt", args.output_dir / "train.data")
        if validation_count:
            run_converter(
                convert_exe,
                args.output_dir / "validation.txt",
                args.output_dir / "validation.data",
            )
        return 0
    except ConversionError as error:
        print(f"to_bullet: {error}", file=sys.stderr)
        return 2
    except OSError as error:
        print(f"to_bullet: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
