#!/usr/bin/env python3
"""Extract every PGN below a directory into a deterministic forensic corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PGN_DIRECTORY = REPOSITORY_ROOT / "third_party" / "User tests"
SCHEMA = "koi-pgn-forensics-v1"

if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools import elo_oracle


class ForensicsError(RuntimeError):
    """An actionable corpus extraction error."""


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _is_inside(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
    except ValueError:
        return False
    return True


def _require_external_output(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if _is_inside(resolved, REPOSITORY_ROOT):
        raise ForensicsError(
            f"generated reports must be outside the repository: '{resolved}'"
        )
    return resolved


def discover_pgns(directory: Path) -> List[Path]:
    """Return all PGNs below a directory in stable relative-path order."""

    try:
        resolved = directory.expanduser().resolve(strict=True)
    except OSError as error:
        raise ForensicsError(f"PGN directory is missing: '{directory}'") from error
    if not resolved.is_dir():
        raise ForensicsError(f"PGN directory is not a directory: '{directory}'")
    paths = [path for path in resolved.rglob("*") if path.is_file() and path.suffix.lower() == ".pgn"]
    return sorted(paths, key=lambda path: path.relative_to(resolved).as_posix().casefold())


def _canonical_hash(files: List[Dict[str, Any]]) -> str:
    canonical = json.dumps(
        {"schema": SCHEMA, "files": files},
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def build_corpus_report(directory: Path) -> Dict[str, Any]:
    """Read and parse every PGN below ``directory`` without starting an engine."""

    resolved_directory = directory.expanduser().resolve(strict=True)
    files: List[Dict[str, Any]] = []
    game_count = 0
    position_count = 0
    for path in discover_pgns(resolved_directory):
        pgn_text, pgn_bytes = elo_oracle._read_pgn(path)
        games = elo_oracle.extract_games(pgn_text)
        file_game_count = len(games)
        file_position_count = sum(len(game["positions"]) for game in games)
        files.append(
            {
                "path": path.relative_to(resolved_directory).as_posix(),
                "sha256": hashlib.sha256(pgn_bytes).hexdigest(),
                "game_count": file_game_count,
                "position_count": file_position_count,
                "games": games,
            }
        )
        game_count += file_game_count
        position_count += file_position_count

    if not files:
        raise ForensicsError(f"no .pgn files found below '{resolved_directory}'")

    return {
        "schema": SCHEMA,
        "schema_version": 1,
        "created_at": _utc_timestamp(),
        "source": {
            "directory": str(resolved_directory),
            "file_count": len(files),
        },
        "file_count": len(files),
        "game_count": game_count,
        "position_count": position_count,
        "content_sha256": _canonical_hash(files),
        "files": files,
    }


def _default_report_path() -> Path:
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%fZ")
    return elo_oracle.default_results_directory() / f"pgn-forensics-{timestamp}-{os.getpid()}.json"


def _write_json(path: Path, report: Dict[str, Any]) -> None:
    external_path = _require_external_output(path)
    try:
        external_path.parent.mkdir(parents=True, exist_ok=True)
        external_path.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    except OSError as error:
        raise ForensicsError(f"unable to write report '{external_path}': {error}") from error


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pgn-dir",
        type=Path,
        default=DEFAULT_PGN_DIRECTORY,
        help="directory to walk recursively for .pgn files",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="external JSON report path (defaults to the system temporary directory)",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    report = build_corpus_report(args.pgn_dir)
    output_path = args.output or _default_report_path()
    _write_json(output_path, report)
    print(f"report {output_path.expanduser().resolve()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ForensicsError, elo_oracle.OracleError) as error:
        print(f"pgn_forensics: error: {error}", file=sys.stderr)
        raise SystemExit(2)
