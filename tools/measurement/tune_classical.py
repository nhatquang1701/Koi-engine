"""Fit classical evaluation term scales from `koi-eval-features` CSV dumps.

The tool is deliberately a report generator: it fits a regularized linear model
of the documented term subset against either the corpus labels (`cp`, side to
move relative) or the evaluator's own `total`, and writes a candidate header
plus a JSON report.  Nothing is adopted automatically - adopting a candidate
requires the repository gates (64/64 tactical suite, full CTest, equal-node
A/B), so this module never edits `src/`.

Usage:
    python tools/measurement/tune_classical.py --input features.csv \
        --header-out artifacts/training/tuned-classical.h --report-out report.json

The input CSV is produced by `koi-eval-features` with the columns
``fen,cp,phase,material,piece_square,...,total``.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import pathlib
import sys
from datetime import datetime, timezone

import numpy as np

TERMS = (
    "material",
    "piece_square",
    "mobility",
    "pawn_structure",
    "activity",
    "development",
    "center_control",
    "initiative",
    "king_safety",
    "king_activity",
    "passed_pawn",
    "tempo",
)
REPORT_SCHEMA = "koi-classical-tuning-report-v1"
HEADER_FORMAT = "koi-classical-tuning-v1"
DEFAULT_RIDGE = 1e-6


class TuningError(RuntimeError):
    """Raised for unusable input or a degenerate fit."""


def load_rows(path: pathlib.Path, limit: int = 0) -> list[dict[str, float | str]]:
    rows: list[dict[str, float | str]] = []
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = [term for term in TERMS if term not in (reader.fieldnames or [])]
        if missing:
            raise TuningError(f"CSV is missing columns: {', '.join(missing)}")
        for raw in reader:
            row: dict[str, float | str] = {}
            try:
                for term in TERMS:
                    row[term] = float(raw[term])
                row["total"] = float(raw["total"])
            except (TypeError, ValueError) as error:
                raise TuningError(f"non-numeric term column: {error}") from error
            cp = (raw.get("cp") or "").strip()
            row["cp"] = float(cp) if cp else None
            row["fen"] = (raw.get("fen") or "").strip()
            rows.append(row)
            if limit > 0 and len(rows) >= limit:
                break
    if not rows:
        raise TuningError("no rows to fit")
    return rows


def fit_scales(
    rows: list[dict[str, float | str]], target: str, ridge: float = DEFAULT_RIDGE
) -> dict[str, float | int | dict[str, float]]:
    usable = [row for row in rows if row.get(target) is not None]
    if len(usable) < len(TERMS) + 1:
        raise TuningError(f"need at least {len(TERMS) + 1} rows with a {target!r} value, got {len(usable)}")
    if ridge < 0:
        raise TuningError("ridge must be non-negative")

    design = np.array([[float(row[term]) for term in TERMS] for row in usable], dtype=float)
    observed = np.array([float(row[target]) for row in usable], dtype=float)

    design_mean = design.mean(axis=0)
    observed_mean = observed.mean()
    centered_design = design - design_mean
    centered_observed = observed - observed_mean
    gram = centered_design.T @ centered_design + ridge * np.eye(len(TERMS))
    scales = np.linalg.solve(gram, centered_design.T @ centered_observed)
    offset = float(observed_mean - design_mean @ scales)

    current_prediction = design.sum(axis=1)
    fitted_prediction = offset + design @ scales
    return {
        "rows": int(len(usable)),
        "target": target,
        "ridge": float(ridge),
        "scales": {term: float(value) for term, value in zip(TERMS, scales)},
        "offset": offset,
        "current": _accuracy(observed, current_prediction),
        "fitted": _accuracy(observed, fitted_prediction),
    }


PIECE_ORDER = "pnbrqk"


def placement_squares(fen: str) -> tuple[bool, list[tuple[int, int, bool]]] | None:
    """Return (side_to_move_is_white, [(piece_index, square, is_own_piece)]).

    The square index matches the evaluator's table indexing: 0 is a1 and 63 is
    h8, and black pieces are mirrored to the white perspective.
    """
    fields = fen.split()
    if len(fields) < 2:
        return None
    placements: list[tuple[int, int, bool]] = []
    white_to_move = fields[1] == "w"
    rank = 7
    file = 0
    for symbol in fields[0]:
        if symbol == "/":
            rank -= 1
            file = 0
            continue
        if symbol.isdigit():
            file += int(symbol)
            continue
        lower = symbol.lower()
        piece_index = PIECE_ORDER.find(lower)
        if piece_index < 0 or file > 7 or rank < 0:
            return None
        square = rank * 8 + file
        is_white = symbol.isupper()
        table_square = square if is_white else (7 - square // 8) * 8 + square % 8
        placements.append((piece_index, table_square, is_white == white_to_move))
        file += 1
    return white_to_move, placements


def fit_piece_square_deltas(
    rows: list[dict[str, float | str]],
    target: str,
    limit_cp: int = 24,
    middle_game_phase: int = 16,
    end_game_phase: int = 8,
) -> dict[str, object]:
    """Fit per-square corrections from the residual of the current evaluation.

    The CSV reports terms from the side to move's perspective, so a positive
    residual means the position is better than the sum of the terms suggests.
    Own pieces therefore receive the residual and enemy pieces receive its
    negation (their term enters the total with a minus sign).  Middle-game and
    end-game buckets are kept apart, the mean correction is removed so the fit
    is zero-sum, and every delta is clamped to +/-`limit_cp`.
    """
    if limit_cp < 0:
        raise TuningError("piece-square limit must be non-negative")
    totals = {0: [0.0] * (6 * 64), 1: [0.0] * (6 * 64)}
    counts = {0: [0] * (6 * 64), 1: [0] * (6 * 64)}
    used = 0
    for row in rows:
        if row.get(target) is None:
            continue
        parsed = placement_squares(str(row.get("fen") or ""))
        if parsed is None:
            continue
        _, placements = parsed
        phase = int(row.get("phase") or 0)
        if phase >= middle_game_phase:
            band = 0
        elif phase <= end_game_phase:
            band = 1
        else:
            continue
        residual = float(row[target]) - float(row["total"])
        for piece_index, square, is_own in placements:
            index = piece_index * 64 + square
            totals[band][index] += residual if is_own else -residual
            counts[band][index] += 1
        used += 1
    if used == 0:
        raise TuningError("no rows with a FEN, a phase, and a target value to fit piece squares")

    deltas: dict[int, list[int]] = {}
    buckets = 0
    for band in (0, 1):
        weighted_total = sum(totals[band])
        weighted_count = sum(counts[band])
        mean = weighted_total / weighted_count if weighted_count else 0.0
        values: list[int] = []
        for index, count in enumerate(counts[band]):
            if count == 0:
                values.append(0)
                continue
            delta = int(round(totals[band][index] / count - mean))
            values.append(max(-limit_cp, min(limit_cp, delta)))
            buckets += 1
        deltas[band] = values
    applied = sum(1 for band in (0, 1) for value in deltas[band] if value != 0)
    return {
        "rows": used,
        "limit_cp": int(limit_cp),
        "buckets": int(buckets),
        "nonzero": int(applied),
        "middle_game_deltas": deltas[0],
        "end_game_deltas": deltas[1],
    }


def piece_square_header(piece_square: dict[str, object], corpus_sha256: str) -> str:
    def rows(values: list[int]) -> list[str]:
        lines: list[str] = []
        for start in range(0, len(values), 8):
            chunk = ", ".join(f"{value:4d}" for value in values[start : start + 8])
            lines.append(f"    {chunk},")
        return lines

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/measurement/tune_classical.py.",
        "// Per-square corrections in centipawns, indexed [piece * 64 + square] with",
        "// the pieces ordered pawn, knight, bishop, rook, queen, king and square 0",
        "// equal to a1 (black pieces are mirrored to the white perspective).  They",
        "// are added to the built-in tables, so adoption requires the 64/64",
        "// tactical gate, the full test suite, and an equal-node A/B report.",
        "",
        "namespace koi {",
        "namespace detail {",
        "",
        f'inline constexpr const char* kTunedPieceSquareCorpusSha256 = "{corpus_sha256}";',
        f"inline constexpr int kTunedPieceSquareRows = {piece_square['rows']};",
        "inline constexpr bool kHasTunedPieceSquares = true;",
        "",
        "inline constexpr std::array<int, 6 * 64> kTunedMiddleGameDeltas{",
    ]
    lines.extend(rows(piece_square["middle_game_deltas"]))  # type: ignore[arg-type]
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr std::array<int, 6 * 64> kTunedEndGameDeltas{")
    lines.extend(rows(piece_square["end_game_deltas"]))  # type: ignore[arg-type]
    lines.append("};")
    lines.append("")
    lines.append("}  // namespace detail")
    lines.append("}  // namespace koi")
    lines.append("")
    return "\n".join(lines)

def _accuracy(observed: np.ndarray, predicted: np.ndarray) -> dict[str, float]:
    residual = observed - predicted
    variance = float(np.sum((observed - observed.mean()) ** 2))
    r2 = 1.0 - float(np.sum(residual**2)) / variance if variance > 0 else 0.0
    return {"mae": float(np.mean(np.abs(residual))), "r2": r2}


def generated_header(report: dict[str, float | int | dict[str, float]], corpus_sha256: str) -> str:
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/measurement/tune_classical.py.",
        "// Candidate multiplicative term scales, NOT adopted weights: adoption",
        "// requires the 64/64 tactical gate, the full test suite, and an",
        "// equal-node A/B report.  The evaluator never reads this header.",
        f'inline constexpr const char* kTunedClassicalMetadataFormatVersion = "{HEADER_FORMAT}";',
        f'inline constexpr const char* kTunedClassicalCorpusSha256 = "{corpus_sha256}";',
        f'inline constexpr int kTunedClassicalRows = {report["rows"]};',
        f'inline constexpr double kTunedClassicalOffset = {report["offset"]:.10g};',
    ]
    scales = report["scales"]
    for term in TERMS:
        symbol = "".join(word.capitalize() for word in term.split("_"))
        lines.append(f"inline constexpr double kTunedClassicalScale{symbol} = {scales[term]:.10g};")
    lines.append("")
    return "\n".join(lines)


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fit classical evaluation term scales from a CSV dump.")
    parser.add_argument("--input", type=pathlib.Path, required=True, help="CSV from koi-eval-features")
    parser.add_argument("--target", choices=("auto", "cp", "total"), default="auto")
    parser.add_argument("--ridge", type=float, default=DEFAULT_RIDGE)
    parser.add_argument("--limit", type=int, default=0, help="fit at most this many rows (0 = all)")
    parser.add_argument("--header-out", type=pathlib.Path)
    parser.add_argument(
        "--pst-header-out",
        type=pathlib.Path,
        help="write per-square corrections that the evaluator reads (koi/evaluation_piece_squares_tuned.hpp)",
    )
    parser.add_argument("--pst-limit", type=int, default=24, help="clamp every piece-square delta to +/- this many centipawns")
    parser.add_argument("--report-out", type=pathlib.Path)
    return parser


def main(argv: list[str]) -> int:
    args = build_parser().parse_args(argv)
    try:
        rows = load_rows(args.input, args.limit)
        target = args.target
        if target == "auto":
            target = "cp" if any(row.get("cp") is not None for row in rows) else "total"
        report = fit_scales(rows, target, args.ridge)
        piece_square = None
        if args.pst_header_out is not None:
            piece_square = fit_piece_square_deltas(rows, target, args.pst_limit)
    except TuningError as error:
        print(f"tune_classical.py: {error}", file=sys.stderr)
        return 2

    corpus_sha256 = sha256_file(args.input)
    report["schema"] = REPORT_SCHEMA
    report["created"] = datetime.now(timezone.utc).isoformat()
    report["corpus_sha256"] = corpus_sha256
    report["input"] = str(args.input)
    report["adopted"] = False

    if args.header_out is not None:
        args.header_out.parent.mkdir(parents=True, exist_ok=True)
        args.header_out.write_text(generated_header(report, corpus_sha256), encoding="utf-8")
    if args.pst_header_out is not None and piece_square is not None:
        args.pst_header_out.parent.mkdir(parents=True, exist_ok=True)
        args.pst_header_out.write_text(piece_square_header(piece_square, corpus_sha256), encoding="utf-8")
        report["piece_square"] = {
            "rows": piece_square["rows"],
            "limit_cp": piece_square["limit_cp"],
            "buckets": piece_square["buckets"],
            "nonzero": piece_square["nonzero"],
            "header": str(args.pst_header_out),
        }
    if args.report_out is not None:
        args.report_out.parent.mkdir(parents=True, exist_ok=True)
        args.report_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    current = report["current"]
    fitted = report["fitted"]
    print(
        f"tuned rows={report['rows']} target={report['target']} "
        f"current mae={current['mae']:.2f} r2={current['r2']:.4f} "
        f"fitted mae={fitted['mae']:.2f} r2={fitted['r2']:.4f}"
    )
    if piece_square is not None:
        print(
            f"piece squares rows={piece_square['rows']} buckets={piece_square['buckets']} "
            f"nonzero={piece_square['nonzero']} limit=+/-{piece_square['limit_cp']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
