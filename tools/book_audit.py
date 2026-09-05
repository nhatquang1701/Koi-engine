#!/usr/bin/env python3
"""Run the licensed opening-book audit using the shared Elo-oracle schema."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List, Optional

try:
    from . import elo_oracle
except ImportError:  # pragma: no cover - used when launched as a script
    import elo_oracle  # type: ignore[no-redef]


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--pgn", required=True, type=Path, help="input standard-SAN PGN path")
    parser.add_argument("--koi", required=True, type=Path, help="Koi UCI executable path")
    parser.add_argument(
        "--stockfish", required=True, type=Path, help="Stockfish UCI executable path"
    )
    parser.add_argument("--book", required=True, type=Path, help="licensed Polyglot book path")
    parser.add_argument(
        "--output",
        type=Path,
        help="output JSON report path (default: external temporary results directory)",
    )
    parser.add_argument(
        "--movetime-ms",
        type=elo_oracle._positive_int,
        default=elo_oracle.DEFAULT_MOVETIME_MS,
        help="per-position UCI movetime",
    )
    parser.add_argument(
        "--threads",
        type=elo_oracle._oracle_threads,
        default=elo_oracle.DEFAULT_THREADS,
        help="oracle thread count; the approved contract requires 4",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    output_path = args.output or elo_oracle.default_report_path("book-audit")
    forwarded = [
        "--pgn",
        str(args.pgn),
        "--koi",
        str(args.koi),
        "--stockfish",
        str(args.stockfish),
        "--book",
        str(args.book),
        "--output",
        str(output_path),
        "--movetime-ms",
        str(args.movetime_ms),
        "--threads",
        str(args.threads),
        "--book-audit",
    ]
    return elo_oracle.main(forwarded)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except elo_oracle.OracleError as error:
        print(f"book_audit: error: {error}", file=sys.stderr)
        raise SystemExit(2)
