#!/usr/bin/env python3
"""Run the repository's reproducible UCI match harness with external output."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional, Sequence

try:
    from .elo_oracle import default_results_directory
except ImportError:  # pragma: no cover - used when launched as a script
    from elo_oracle import default_results_directory  # type: ignore[no-redef]


class MatchToolError(RuntimeError):
    """An actionable match-tool configuration or process error."""


STOCKFISH_MIN_ELO = 1320
STOCKFISH_MAX_ELO = 3190


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _nonnegative_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def _stockfish_elo(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    return max(STOCKFISH_MIN_ELO, min(STOCKFISH_MAX_ELO, parsed))


def _time_control(value: str) -> str:
    if re.fullmatch(r"[1-9][0-9]*\+[0-9]+", value) is None:
        raise argparse.ArgumentTypeError("must use <minutes>+<increment>, for example 5+3")
    return value


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--koi", required=True, type=Path, help="Koi UCI executable path")
    parser.add_argument(
        "--stockfish", required=True, type=Path, help="opponent UCI executable path"
    )
    parser.add_argument("--replay-path", type=Path, help="koi-replay executable path")
    positions = parser.add_mutually_exclusive_group()
    positions.add_argument("--fen-file", type=Path, help="named FEN suite path")
    positions.add_argument("--opening-file", type=Path, help="named UCI opening suite path")
    limits = parser.add_mutually_exclusive_group()
    limits.add_argument("--time-control", type=_time_control, help="clock in <minutes>+<increment>")
    limits.add_argument("--movetime-ms", type=_positive_int, help="fixed UCI movetime per search")
    limits.add_argument("--nodes", type=_positive_int, help="fixed UCI node limit per search")
    parser.add_argument("--depth", type=_positive_int, default=4, help="fixed depth when no other limit is set")
    parser.add_argument("--games", type=_positive_int, default=1, help="games per position")
    parser.add_argument("--random-seed", type=_nonnegative_int, default=1, help="Koi RandomSeed")
    parser.add_argument(
        "--own-book",
        choices=("true", "false"),
        default="true",
        help="Koi OwnBook value",
    )
    parser.add_argument("--book-file", default="book.bin", help="Koi BookFile value")
    parser.add_argument("--book-depth", type=int, default=16, help="Koi BookDepth value")
    parser.add_argument("--book-random", choices=("true", "false"), default="false", help="Koi BookRandom value")
    parser.add_argument("--threads", type=_positive_int, default=1, help="Koi and opponent Threads")
    parser.add_argument("--speed", type=_positive_int, default=100, help="Koi Speed")
    parser.add_argument("--hash", type=_positive_int, default=16, dest="hash_mb", help="Koi Hash in MiB")
    parser.add_argument(
        "--opponent-elo",
        "--stockfish-elo",
        dest="opponent_elo",
        type=_stockfish_elo,
        help="Stockfish UCI_Elo anchor; enables UCI_LimitStrength",
    )
    parser.add_argument("--max-plies", type=_positive_int, default=512, help="maximum plies per game")
    parser.add_argument(
        "--timeout-ms",
        type=_positive_int,
        default=5000,
        help="protocol read/ready timeout; clocked searches use --time-control as their deadline",
    )
    parser.add_argument("--koi-color", choices=("white", "black"), default="white")
    parser.add_argument(
        "--output-directory",
        type=Path,
        default=default_results_directory() / "matches",
        help="directory for JSON and PGN reports",
    )
    parser.add_argument(
        "--batch-id",
        default="batch-00",
        help="paired schedule batch identity recorded in the match report",
    )
    parser.add_argument(
        "--run-label",
        choices=("measurement", "before", "after"),
        default="measurement",
        help="paired-run label recorded in the match report",
    )
    return parser


def _find_powershell() -> str:
    for candidate in ("pwsh", "powershell"):
        resolved = shutil.which(candidate)
        if resolved:
            return resolved
    raise MatchToolError("PowerShell (pwsh or powershell) is required to run uci_match.ps1.")


def _append(command: List[str], name: str, value: object) -> None:
    command.extend((name, str(value)))


def build_command(args: argparse.Namespace, powershell: Optional[str] = None) -> List[str]:
    """Build a shell-free command for the existing PowerShell match harness."""

    script = Path(__file__).with_name("uci_match.ps1").resolve()
    if not script.is_file():
        raise MatchToolError(f"match harness is missing: {script}")

    command = [
        powershell or _find_powershell(),
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script),
    ]
    _append(command, "-KoiPath", args.koi)
    _append(command, "-OpponentPath", args.stockfish)
    _append(command, "-OutputDirectory", args.output_directory)
    if args.replay_path is not None:
        _append(command, "-ReplayPath", args.replay_path)
    if args.fen_file is not None:
        _append(command, "-FenFile", args.fen_file)
    if args.opening_file is not None:
        _append(command, "-OpeningFile", args.opening_file)
    if args.time_control is not None:
        _append(command, "-TimeControl", args.time_control)
    elif args.movetime_ms is not None:
        _append(command, "-MovetimeMs", args.movetime_ms)
    elif args.nodes is not None:
        _append(command, "-Nodes", args.nodes)
    else:
        _append(command, "-Depth", args.depth)
    _append(command, "-Games", args.games)
    _append(command, "-KoiRandomSeed", args.random_seed)
    _append(command, "-KoiOwnBook", args.own_book)
    _append(command, "-KoiBookFile", args.book_file)
    _append(command, "-KoiBookDepth", args.book_depth)
    _append(command, "-KoiBookRandom", args.book_random)
    _append(command, "-Threads", args.threads)
    _append(command, "-Speed", args.speed)
    _append(command, "-Hash", args.hash_mb)
    if getattr(args, "opponent_elo", None) is not None:
        _append(command, "-OpponentElo", args.opponent_elo)
    _append(command, "-MaxPlies", args.max_plies)
    _append(command, "-TimeoutMilliseconds", args.timeout_ms)
    _append(command, "-KoiColor", args.koi_color)
    _append(command, "-BatchId", args.batch_id)
    _append(command, "-RunLabel", args.run_label)
    return command


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        completed = subprocess.run(build_command(args), check=False)
    except (MatchToolError, OSError) as error:
        print(f"stockfish_match: error: {error}", file=sys.stderr)
        return 2
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
