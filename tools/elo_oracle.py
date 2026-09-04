#!/usr/bin/env python3
"""Extract PGN positions and compare Koi moves with a Stockfish oracle."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import queue
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


SCHEMA = "koi-elo-oracle"
SCHEMA_VERSION = 1
MATE_SCORE_CP = 100000
DEFAULT_MOVETIME_MS = 250
DEFAULT_THREADS = 4
DEFAULT_ENGINE_TIMEOUT_SECONDS = 30
BOOK_DEPTH = 16
BOOK_RANDOM = False
ENGINE_VERSION_RE = re.compile(r"\b(?:v)?(\d+(?:\.\d+)+(?:[-+._A-Za-z0-9]*)?)\b")
UCI_MOVE_RE = re.compile(r"^(?:[a-h][1-8][a-h][1-8](?:[nbrq])?|0000)$")
BOOK_INFO_RE = re.compile(
    r"^info string book move (?P<move>[a-h][1-8][a-h][1-8][nbrq]?)\s+depth\s+(?P<ply>\d+)\s*$"
)
BESTMOVE_RE = re.compile(
    r"^bestmove\s+(?P<move>\S+)(?:\s+ponder\s+(?P<ponder>\S+))?\s*$"
)
INFO_RE = re.compile(r"^info(?:\s|$)")
SCORE_RE = re.compile(
    r"(?:^|\s)score\s+(?P<type>cp|mate)\s+(?P<value>-?\d+)"
    r"(?:\s+(?P<bound>lowerbound|upperbound))?(?=\s|$)"
)


class OracleError(RuntimeError):
    """An actionable error caused by input, configuration, or an engine."""


@dataclass(frozen=True)
class UciScore:
    score_type: str
    value: int
    normalized_cp: int
    root_side: str


@dataclass(frozen=True)
class ParsedInfo:
    score: UciScore
    depth: Optional[int]
    seldepth: Optional[int]
    nodes: Optional[int]
    nps: Optional[int]
    time_ms: Optional[int]
    multipv: Optional[int]
    bound: Optional[str]
    pv: List[str]
    raw: str


@dataclass(frozen=True)
class SearchResult:
    bestmove: str
    ponder: Optional[str]
    elapsed_ms: int
    final_info: Optional[ParsedInfo]
    info_count: int
    book_used: bool
    book_move: Optional[str]
    book_ply: Optional[int]


def normalize_score(score_type: str, value: int, root_side: str) -> int:
    """Normalize a UCI score to centipawns from White's perspective."""

    if root_side not in {"white", "black"}:
        raise ValueError(f"invalid root side: {root_side}")
    if score_type == "cp":
        score_cp = int(value)
    elif score_type == "mate":
        # UCI's zero-distance mate is the side-to-move being mated.
        score_cp = MATE_SCORE_CP if int(value) > 0 else -MATE_SCORE_CP
    else:
        raise ValueError(f"unsupported UCI score type: {score_type}")
    return score_cp if root_side == "white" else -score_cp


def calculate_cpl(root_score_white_cp: int, resulting_score_white_cp: int, moving_side: str) -> int:
    """Return centipawn loss from the side that made the candidate move."""

    if moving_side not in {"white", "black"}:
        raise ValueError(f"invalid moving side: {moving_side}")
    loss = (
        root_score_white_cp - resulting_score_white_cp
        if moving_side == "white"
        else resulting_score_white_cp - root_score_white_cp
    )
    return max(0, int(loss))


def parse_info_line(line: str, root_side: str) -> Optional[ParsedInfo]:
    """Parse a scored UCI info line and ignore non-scored info lines."""

    if not INFO_RE.match(line) or BOOK_INFO_RE.fullmatch(line):
        return None
    score_match = SCORE_RE.search(line)
    if score_match is None:
        return None

    def integer_field(name: str) -> Optional[int]:
        match = re.search(rf"(?:^|\s){name}\s+(-?\d+)(?=\s|$)", line)
        return int(match.group(1)) if match else None

    pv_match = re.search(r"(?:^|\s)pv\s+(.+)$", line)
    score_type = score_match.group("type")
    score_value = int(score_match.group("value"))
    return ParsedInfo(
        score=UciScore(
            score_type=score_type,
            value=score_value,
            normalized_cp=normalize_score(score_type, score_value, root_side),
            root_side=root_side,
        ),
        depth=integer_field("depth"),
        seldepth=integer_field("seldepth"),
        nodes=integer_field("nodes"),
        nps=integer_field("nps"),
        time_ms=integer_field("time"),
        multipv=integer_field("multipv"),
        bound=score_match.group("bound"),
        pv=pv_match.group(1).strip().split() if pv_match else [],
        raw=line,
    )


def parse_book_info_line(line: str) -> Optional[Tuple[str, int]]:
    """Parse Koi's book marker without treating it as search info."""

    match = BOOK_INFO_RE.fullmatch(line)
    if match is None:
        return None
    return match.group("move").lower(), int(match.group("ply"))


def parse_bestmove_line(line: str) -> Optional[Tuple[str, Optional[str]]]:
    """Parse a coordinate UCI bestmove line."""

    match = BESTMOVE_RE.fullmatch(line)
    if match is None:
        return None
    move = match.group("move").lower()
    ponder = match.group("ponder")
    if not UCI_MOVE_RE.fullmatch(move):
        raise OracleError(f"Invalid UCI bestmove: {line}")
    if ponder is not None:
        ponder = ponder.lower()
        if not UCI_MOVE_RE.fullmatch(ponder):
            raise OracleError(f"Invalid UCI ponder move: {line}")
    return move, ponder


def _load_chess_modules() -> Tuple[Any, Any]:
    try:
        import chess
        import chess.pgn
    except ImportError as error:
        raise OracleError(
            "python-chess is required for PGN extraction; install it with "
            "'python -m pip install python-chess'."
        ) from error
    return chess, chess.pgn


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _read_pgn(path: Path) -> Tuple[str, bytes]:
    try:
        data = path.read_bytes()
    except OSError as error:
        raise OracleError(f"Unable to read PGN file '{path}': {error}") from error
    try:
        return data.decode("utf-8-sig"), data
    except UnicodeDecodeError as error:
        raise OracleError(f"PGN file is not valid UTF-8: '{path}'") from error


def extract_games(pgn_text: str) -> List[Dict[str, Any]]:
    """Return every PGN game's mainline positions in source order."""

    chess, pgn_module = _load_chess_modules()
    from io import StringIO

    games: List[Dict[str, Any]] = []
    stream = StringIO(pgn_text)
    while True:
        game = pgn_module.read_game(stream)
        if game is None:
            break
        if game.errors:
            details = "; ".join(str(error) for error in game.errors)
            raise OracleError(f"PGN contains parse errors: {details}")

        board = game.board()
        positions: List[Dict[str, Any]] = []
        for ply, node in enumerate(game.mainline(), start=1):
            move = node.move
            if move is None:
                raise OracleError(f"PGN mainline contains a missing move at ply {ply}.")
            try:
                san = board.san(move)
            except ValueError as error:
                raise OracleError(f"PGN mainline contains an illegal move at ply {ply}.") from error

            positions.append(
                {
                    "ply": ply,
                    "move_number": board.fullmove_number,
                    "side": "white" if board.turn == chess.WHITE else "black",
                    "fen": board.fen(),
                    "actual_move_uci": move.uci(),
                    "actual_move_san": san,
                }
            )
            board.push(move)

        games.append(
            {
                "game_index": len(games) + 1,
                "headers": dict(game.headers),
                "positions": positions,
            }
        )

    if not games:
        raise OracleError("PGN contains no games.")
    return games


def build_extraction_report(
    pgn_path: Path,
    pgn_text: str,
    games: List[Dict[str, Any]],
    movetime_ms: int,
    threads: int,
    pgn_sha256: Optional[str] = None,
) -> Dict[str, Any]:
    created_at = _utc_timestamp()
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "mode": "extract-only",
        "created_at": created_at,
        "source": {
            "pgn_path": str(pgn_path),
            "pgn_sha256": pgn_sha256 or hashlib.sha256(pgn_text.encode("utf-8")).hexdigest(),
        },
        "evaluation": {
            "perspective": "white",
            "mate_score_cp": MATE_SCORE_CP,
            "cpl_definition": "max(0, mover_perspective(root_score - resulting_position_score))",
        },
        "requested_limits": {
            "movetime_ms": movetime_ms,
            "threads": threads,
            "speed": 100,
        },
        "engines": {},
        "games": games,
    }


def _json_options(options: Dict[str, Any]) -> Dict[str, Any]:
    return {key: value for key, value in options.items()}


class UciEngine:
    """Small line-oriented UCI client with shell-free process handling."""

    def __init__(self, path: Path, label: str):
        self.path = path
        self.label = label
        self.process: Optional[subprocess.Popen[str]] = None
        self._stdout_queue: "queue.Queue[Optional[str]]" = queue.Queue()
        self._stderr_lines: List[str] = []
        self._stdout_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self.handshake_lines: List[str] = []
        self.option_lines: List[str] = []
        self.id_name: Optional[str] = None
        self.id_author: Optional[str] = None

    def __enter__(self) -> "UciEngine":
        self.start()
        return self

    def __exit__(self, _exception_type: Any, _exception: Any, _traceback: Any) -> None:
        self.close()

    def start(self) -> None:
        if self.process is not None:
            return
        popen_kwargs: Dict[str, Any] = {
            "args": [str(self.path)],
            "stdin": subprocess.PIPE,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            "text": True,
            "encoding": "utf-8",
            "errors": "replace",
            "bufsize": 1,
        }
        if sys.platform == "win32":
            popen_kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        try:
            self.process = subprocess.Popen(**popen_kwargs)
        except OSError as error:
            raise OracleError(
                f"Unable to start {self.label} executable '{self.path}': {error}"
            ) from error

        assert self.process.stdout is not None
        assert self.process.stderr is not None
        self._stdout_thread = threading.Thread(target=self._pump_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._pump_stderr, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()

        self._send("uci")
        while True:
            line = self._read_line("uciok")
            self.handshake_lines.append(line)
            if line == "uciok":
                break
            name_match = re.match(r"^id name (.+)$", line)
            author_match = re.match(r"^id author (.+)$", line)
            if name_match:
                self.id_name = name_match.group(1).strip()
            elif author_match:
                self.id_author = author_match.group(1).strip()
            elif line.startswith("option "):
                self.option_lines.append(line)
        self._wait_ready()

    def _pump_stdout(self) -> None:
        assert self.process is not None
        assert self.process.stdout is not None
        for line in iter(self.process.stdout.readline, ""):
            self._stdout_queue.put(line.rstrip("\r\n"))
        self._stdout_queue.put(None)

    def _pump_stderr(self) -> None:
        assert self.process is not None
        assert self.process.stderr is not None
        for line in iter(self.process.stderr.readline, ""):
            self._stderr_lines.append(line.rstrip("\r\n"))

    def _read_line(self, description: str) -> str:
        try:
            line = self._stdout_queue.get(timeout=DEFAULT_ENGINE_TIMEOUT_SECONDS)
        except queue.Empty as error:
            raise OracleError(
                f"Timed out waiting for {self.label} {description} from '{self.path}'."
            ) from error
        if line is None:
            exit_code = self.process.poll() if self.process is not None else None
            stderr = " | ".join(self._stderr_lines[-3:])
            detail = f" stderr: {stderr}" if stderr else ""
            raise OracleError(
                f"{self.label} closed stdout while waiting for {description} "
                f"(exit code {exit_code}).{detail}"
            )
        return line

    def _send(self, command: str) -> None:
        if self.process is None or self.process.poll() is not None:
            raise OracleError(f"{self.label} exited before command '{command}'.")
        assert self.process.stdin is not None
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
        except OSError as error:
            raise OracleError(f"Unable to send '{command}' to {self.label}: {error}") from error

    def _wait_ready(self) -> None:
        self._send("isready")
        while True:
            if self._read_line("readyok") == "readyok":
                return

    @staticmethod
    def _format_option_value(value: Any) -> str:
        if isinstance(value, bool):
            return "true" if value else "false"
        return str(value)

    def _set_options(self, options: Dict[str, Any]) -> None:
        for name, value in options.items():
            self._send(
                f"setoption name {name} value {self._format_option_value(value)}"
            )

    def search(
        self,
        fen: str,
        root_side: str,
        movetime_ms: int,
        options: Dict[str, Any],
        require_score: bool,
    ) -> SearchResult:
        self._set_options(options)
        self._wait_ready()
        self._send(f"position fen {fen}")
        started = time.perf_counter()
        self._send(f"go movetime {movetime_ms}")

        infos: List[ParsedInfo] = []
        book_used = False
        book_move: Optional[str] = None
        book_ply: Optional[int] = None
        bestmove: Optional[str] = None
        ponder: Optional[str] = None
        while bestmove is None:
            line = self._read_line("bestmove")
            book_marker = parse_book_info_line(line)
            if book_marker is not None:
                if book_used:
                    raise OracleError(
                        f"{self.label} emitted more than one book marker for FEN '{fen}'."
                    )
                book_used = True
                book_move, book_ply = book_marker
                continue
            info = parse_info_line(line, root_side)
            if info is not None:
                infos.append(info)
                continue
            parsed_bestmove = parse_bestmove_line(line)
            if parsed_bestmove is None:
                # UCI permits other informational lines. They are deliberately
                # ignored instead of being treated as engine search data.
                continue
            bestmove, ponder = parsed_bestmove

        elapsed_ms = max(0, int(round((time.perf_counter() - started) * 1000)))
        final_info = infos[-1] if infos else None
        if require_score and final_info is None:
            raise OracleError(
                f"{self.label} did not emit a scored UCI info line before bestmove for FEN '{fen}'."
            )
        return SearchResult(
            bestmove=bestmove,
            ponder=ponder,
            elapsed_ms=elapsed_ms,
            final_info=final_info,
            info_count=len(infos),
            book_used=book_used,
            book_move=book_move,
            book_ply=book_ply,
        )

    @staticmethod
    def _version_from_identity(identity: Optional[str]) -> str:
        if not identity:
            return "unknown"
        match = ENGINE_VERSION_RE.search(identity)
        return match.group(1) if match else "unknown"

    def metadata(self, options: Dict[str, Any]) -> Dict[str, Any]:
        return {
            "path": str(self.path),
            "identity": {
                "name": self.id_name or "unknown",
                "author": self.id_author,
            },
            "version": self._version_from_identity(self.id_name),
            "options": _json_options(options),
            "uci_handshake": list(self.handshake_lines),
            "advertised_options": list(self.option_lines),
        }

    def close(self) -> None:
        process = self.process
        if process is None:
            return
        if process.poll() is None:
            try:
                self._send("quit")
            except OracleError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        self.process = None


def _resolve_file(raw_path: Optional[Path], label: str, option: str) -> Path:
    if raw_path is None:
        raise OracleError(f"{label} requires {option}.")
    try:
        resolved = raw_path.expanduser().resolve(strict=True)
    except OSError as error:
        raise OracleError(f"{label} file is missing: '{raw_path}'.") from error
    if not resolved.is_file():
        raise OracleError(f"{label} file is missing: '{raw_path}'.")
    return resolved


def _resolve_engine_paths(
    koi_raw: Optional[Path], stockfish_raw: Optional[Path], mode: str
) -> Tuple[Path, Path]:
    missing: List[str] = []
    if koi_raw is None:
        missing.append("--koi <koi-engine.exe>")
    if stockfish_raw is None:
        missing.append("--stockfish <stockfish.exe>")
    if missing:
        raise OracleError(f"{mode} requires " + " and ".join(missing) + ".")
    return (
        _resolve_file(koi_raw, "Koi executable", "--koi <koi-engine.exe>"),
        _resolve_file(stockfish_raw, "Stockfish executable", "--stockfish <stockfish.exe>"),
    )


def _side_from_fen(fen: str) -> str:
    fields = fen.split()
    if len(fields) < 2 or fields[1] not in {"w", "b"}:
        raise OracleError(f"Position FEN has no valid side-to-move field: '{fen}'.")
    return "white" if fields[1] == "w" else "black"


def _child_position(
    chess: Any, fen: str, move_uci: str, description: str
) -> Tuple[str, str]:
    if not UCI_MOVE_RE.fullmatch(move_uci):
        raise OracleError(f"{description} returned an invalid UCI move '{move_uci}'.")
    try:
        board = chess.Board(fen)
        move = chess.Move.from_uci(move_uci)
    except ValueError as error:
        raise OracleError(f"{description} returned move '{move_uci}' for invalid FEN '{fen}'.") from error
    if not board.is_legal(move):
        raise OracleError(f"{description} returned illegal move '{move_uci}' for FEN '{fen}'.")
    san = board.san(move)
    board.push(move)
    return board.fen(), san


def _score_dict(score: Optional[UciScore]) -> Optional[Dict[str, Any]]:
    if score is None:
        return None
    return {
        "type": score.score_type,
        "value": score.value,
        "normalized_cp": score.normalized_cp,
        "perspective": "white",
        "root_side": score.root_side,
    }


def _result_dict(result: SearchResult, include_book: bool = False) -> Dict[str, Any]:
    info = result.final_info
    data: Dict[str, Any] = {
        "bestmove": result.bestmove,
        "ponder": result.ponder,
        "elapsed_ms": result.elapsed_ms,
        "info_count": result.info_count,
        "score": _score_dict(info.score if info else None),
        "depth": info.depth if info else None,
        "seldepth": info.seldepth if info else None,
        "nodes": info.nodes if info else None,
        "nps": info.nps if info else None,
        "engine_time_ms": info.time_ms if info else None,
        "pv": list(info.pv) if info else [],
    }
    if include_book:
        data.update(
            {
                "book_used": result.book_used,
                "book_move": result.book_move,
                "book_ply": result.book_ply,
            }
        )
    return data


def _scored_result(result: SearchResult, label: str) -> UciScore:
    if result.final_info is None:
        raise OracleError(f"{label} did not produce a score.")
    return result.final_info.score


def _metric_summary(values: Sequence[int]) -> Dict[str, Any]:
    if not values:
        return {
            "count": 0,
            "mean_cpl": None,
            "p95_cpl": None,
            "blunders_at_least_100_cp": 0,
        }
    sorted_values = sorted(int(value) for value in values)
    p95_index = min(
        len(sorted_values) - 1,
        max(0, math.ceil(0.95 * len(sorted_values)) - 1),
    )
    return {
        "count": len(sorted_values),
        "mean_cpl": round(sum(sorted_values) / len(sorted_values), 2),
        "p95_cpl": sorted_values[p95_index],
        "blunders_at_least_100_cp": sum(value >= 100 for value in sorted_values),
    }


def _report_header(
    mode: str,
    pgn_path: Path,
    pgn_sha256: str,
    started_at: str,
    finished_at: str,
    movetime_ms: int,
    threads: int,
    engines: Dict[str, Any],
) -> Dict[str, Any]:
    return {
        "schema": SCHEMA,
        "schema_version": SCHEMA_VERSION,
        "mode": mode,
        "created_at": started_at,
        "started_at": started_at,
        "finished_at": finished_at,
        "source": {
            "pgn_path": str(pgn_path),
            "pgn_sha256": pgn_sha256,
        },
        "evaluation": {
            "perspective": "white",
            "mate_score_cp": MATE_SCORE_CP,
            "cpl_definition": "max(0, mover_perspective(root_score - resulting_position_score))",
        },
        "requested_limits": {
            "movetime_ms": movetime_ms,
            "threads": threads,
            "speed": 100,
            "koi_go": f"go movetime {movetime_ms}",
            "stockfish_go": f"go movetime {movetime_ms}",
        },
        "engines": engines,
    }


def _position_record(position: Dict[str, Any]) -> Dict[str, Any]:
    return copy.deepcopy(position)


def run_search_analysis(
    games: List[Dict[str, Any]],
    pgn_path: Path,
    pgn_sha256: str,
    koi_path: Path,
    stockfish_path: Path,
    movetime_ms: int,
    threads: int,
) -> Dict[str, Any]:
    chess, _pgn_module = _load_chess_modules()
    started_at = _utc_timestamp()
    koi_options: Dict[str, Any] = {
        "OwnBook": False,
        "Threads": threads,
        "Speed": 100,
    }
    stockfish_options: Dict[str, Any] = {"Threads": threads}
    result_games: List[Dict[str, Any]] = []
    actual_cpls: List[int] = []
    koi_cpls: List[int] = []

    with UciEngine(koi_path, "Koi") as koi, UciEngine(stockfish_path, "Stockfish") as stockfish:
        for game in games:
            result_positions: List[Dict[str, Any]] = []
            for position in game["positions"]:
                fen = position["fen"]
                root_side = _side_from_fen(fen)
                if root_side != position["side"]:
                    raise OracleError(
                        f"Extracted side '{position['side']}' disagrees with FEN side '{root_side}' at ply {position['ply']}."
                    )

                koi_result = koi.search(
                    fen, root_side, movetime_ms, koi_options, require_score=False
                )
                koi_child_fen, koi_san = _child_position(
                    chess, fen, koi_result.bestmove, "Koi"
                )
                root_result = stockfish.search(
                    fen, root_side, movetime_ms, stockfish_options, require_score=True
                )
                actual_child_fen, actual_san = _child_position(
                    chess, fen, position["actual_move_uci"], "PGN actual move"
                )
                actual_child_side = _side_from_fen(actual_child_fen)
                koi_child_side = _side_from_fen(koi_child_fen)
                actual_result = stockfish.search(
                    actual_child_fen,
                    actual_child_side,
                    movetime_ms,
                    stockfish_options,
                    require_score=True,
                )
                koi_child_result = stockfish.search(
                    koi_child_fen,
                    koi_child_side,
                    movetime_ms,
                    stockfish_options,
                    require_score=True,
                )

                root_score = _scored_result(root_result, "Stockfish root search")
                actual_score = _scored_result(actual_result, "Stockfish actual-move search")
                koi_score = _scored_result(koi_child_result, "Stockfish Koi-move search")
                actual_cpl = calculate_cpl(
                    root_score.normalized_cp,
                    actual_score.normalized_cp,
                    position["side"],
                )
                koi_cpl = calculate_cpl(
                    root_score.normalized_cp,
                    koi_score.normalized_cp,
                    position["side"],
                )
                actual_cpls.append(actual_cpl)
                koi_cpls.append(koi_cpl)

                record = _position_record(position)
                record["search"] = {
                    "koi_search": _result_dict(koi_result),
                    "stockfish_root": _result_dict(root_result),
                    "actual_game_move": {
                        "uci": position["actual_move_uci"],
                        "san": actual_san,
                        "resulting_fen": actual_child_fen,
                        "stockfish_resulting_position": _result_dict(actual_result),
                        "cpl": actual_cpl,
                    },
                    "koi_suggested_move": {
                        "uci": koi_result.bestmove,
                        "san": koi_san,
                        "resulting_fen": koi_child_fen,
                        "stockfish_resulting_position": _result_dict(koi_child_result),
                        "cpl": koi_cpl,
                    },
                    "timings_ms": {
                        "koi_search": koi_result.elapsed_ms,
                        "stockfish_root": root_result.elapsed_ms,
                        "stockfish_actual_move": actual_result.elapsed_ms,
                        "stockfish_koi_move": koi_child_result.elapsed_ms,
                    },
                }
                result_positions.append(record)
            result_games.append(
                {
                    "game_index": game["game_index"],
                    "headers": copy.deepcopy(game["headers"]),
                    "positions": result_positions,
                }
            )

        finished_at = _utc_timestamp()
        engines = {
            "koi": koi.metadata(koi_options),
            "stockfish": stockfish.metadata(stockfish_options),
        }

    report = _report_header(
        "analysis",
        pgn_path,
        pgn_sha256,
        started_at,
        finished_at,
        movetime_ms,
        threads,
        engines,
    )
    report["search_metrics"] = {
        "includes_book_hits": False,
        "positions": len(actual_cpls),
        "actual_game_move": _metric_summary(actual_cpls),
        "koi_suggested_move": _metric_summary(koi_cpls),
    }
    report["games"] = result_games
    return report


def run_book_audit(
    games: List[Dict[str, Any]],
    pgn_path: Path,
    pgn_sha256: str,
    koi_path: Path,
    stockfish_path: Path,
    book_path: Path,
    movetime_ms: int,
    threads: int,
) -> Dict[str, Any]:
    chess, _pgn_module = _load_chess_modules()
    started_at = _utc_timestamp()
    koi_options: Dict[str, Any] = {
        "OwnBook": True,
        "BookFile": str(book_path),
        "BookDepth": BOOK_DEPTH,
        "BookRandom": BOOK_RANDOM,
        "Threads": threads,
        "Speed": 100,
    }
    stockfish_options: Dict[str, Any] = {"Threads": threads}
    result_games: List[Dict[str, Any]] = []
    book_cpls: List[int] = []
    actual_cpls: List[int] = []
    book_hits = 0

    with UciEngine(koi_path, "Koi") as koi, UciEngine(stockfish_path, "Stockfish") as stockfish:
        for game in games:
            result_positions: List[Dict[str, Any]] = []
            for position in game["positions"]:
                fen = position["fen"]
                root_side = _side_from_fen(fen)
                koi_result = koi.search(
                    fen, root_side, movetime_ms, koi_options, require_score=False
                )
                root_result = stockfish.search(
                    fen, root_side, movetime_ms, stockfish_options, require_score=True
                )
                actual_child_fen, actual_san = _child_position(
                    chess, fen, position["actual_move_uci"], "PGN actual move"
                )
                actual_child_result = stockfish.search(
                    actual_child_fen,
                    _side_from_fen(actual_child_fen),
                    movetime_ms,
                    stockfish_options,
                    require_score=True,
                )
                root_score = _scored_result(root_result, "Stockfish root search")
                actual_score = _scored_result(
                    actual_child_result, "Stockfish actual-move search"
                )
                actual_cpl = calculate_cpl(
                    root_score.normalized_cp,
                    actual_score.normalized_cp,
                    position["side"],
                )
                actual_cpls.append(actual_cpl)

                book_move: Optional[str] = None
                book_ply: Optional[int] = None
                book_san: Optional[str] = None
                book_child_fen: Optional[str] = None
                book_result: Optional[SearchResult] = None
                book_cpl: Optional[int] = None
                if koi_result.book_used:
                    book_hits += 1
                    book_move = koi_result.book_move
                    book_ply = koi_result.book_ply
                    if book_move is None or book_ply is None:
                        raise OracleError(
                            f"Koi reported book_used without a move and ply at game {game['game_index']} ply {position['ply']}."
                        )
                    if koi_result.bestmove != book_move:
                        raise OracleError(
                            f"Koi book marker '{book_move}' disagrees with bestmove '{koi_result.bestmove}'."
                        )
                    book_child_fen, book_san = _child_position(
                        chess, fen, book_move, "Koi book"
                    )
                    book_result = stockfish.search(
                        book_child_fen,
                        _side_from_fen(book_child_fen),
                        movetime_ms,
                        stockfish_options,
                        require_score=True,
                    )
                    book_score = _scored_result(book_result, "Stockfish book-move search")
                    book_cpl = calculate_cpl(
                        root_score.normalized_cp,
                        book_score.normalized_cp,
                        position["side"],
                    )
                    book_cpls.append(book_cpl)

                record = _position_record(position)
                record["book_audit"] = {
                    "book_used": koi_result.book_used,
                    "book_move": book_move,
                    "book_ply": book_ply,
                    "book_move_san": book_san,
                    "stockfish_cpl": book_cpl,
                    "actual_game_move_stockfish_cpl": actual_cpl,
                    "koi_book_search": _result_dict(koi_result, include_book=True),
                    "stockfish_root": _result_dict(root_result),
                    "timings_ms": {
                        "koi_book_search": koi_result.elapsed_ms,
                        "stockfish_root": root_result.elapsed_ms,
                        "stockfish_actual_move": actual_child_result.elapsed_ms,
                        "stockfish_book_move": book_result.elapsed_ms if book_result else None,
                    },
                }
                if book_result is not None:
                    record["book_audit"]["book_resulting_fen"] = book_child_fen
                    record["book_audit"]["stockfish_book_resulting_position"] = _result_dict(book_result)
                result_positions.append(record)
            result_games.append(
                {
                    "game_index": game["game_index"],
                    "headers": copy.deepcopy(game["headers"]),
                    "positions": result_positions,
                }
            )

        finished_at = _utc_timestamp()
        engines = {
            "koi": koi.metadata(koi_options),
            "stockfish": stockfish.metadata(stockfish_options),
        }

    report = _report_header(
        "book-audit",
        pgn_path,
        pgn_sha256,
        started_at,
        finished_at,
        movetime_ms,
        threads,
        engines,
    )
    report["search_metrics"] = None
    report["book_audit"] = {
        "includes_search_metrics": False,
        "settings": {
            "OwnBook": True,
            "BookFile": str(book_path),
            "BookDepth": BOOK_DEPTH,
            "BookRandom": BOOK_RANDOM,
        },
        "positions": sum(len(game["positions"]) for game in result_games),
        "book_hits": book_hits,
        "stockfish_cpl": _metric_summary(book_cpls),
        "actual_game_move_stockfish_cpl": _metric_summary(actual_cpls),
    }
    report["games"] = result_games
    return report


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def _oracle_threads(value: str) -> int:
    parsed = _positive_int(value)
    if parsed != DEFAULT_THREADS:
        raise argparse.ArgumentTypeError(
            f"must be exactly {DEFAULT_THREADS} for the approved Elo oracle contract"
        )
    return parsed


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgn", required=True, type=Path, help="input PGN path")
    parser.add_argument("--koi", type=Path, help="Koi UCI executable path")
    parser.add_argument("--stockfish", type=Path, help="Stockfish UCI executable path")
    parser.add_argument("--output", required=True, type=Path, help="output JSON report path")
    parser.add_argument("--movetime-ms", type=_positive_int, default=DEFAULT_MOVETIME_MS)
    parser.add_argument(
        "--threads",
        type=_oracle_threads,
        default=DEFAULT_THREADS,
        help="oracle thread count; the approved contract requires 4",
    )
    parser.add_argument(
        "--book",
        type=Path,
        help="licensed Polyglot book path; selects book-audit mode",
    )
    parser.add_argument(
        "--extract-only",
        action="store_true",
        help="extract PGN positions without engines or a book",
    )
    parser.add_argument(
        "--book-audit",
        action="store_true",
        help="run the separate licensed-book audit mode",
    )
    return parser


def _write_json(path: Path, report: Dict[str, Any]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    except OSError as error:
        raise OracleError(f"Unable to write report '{path}': {error}") from error


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.extract_only and args.book_audit:
        raise OracleError("--extract-only and --book-audit cannot be combined")

    pgn_path = _resolve_file(args.pgn, "PGN", "--pgn <game.pgn>")
    pgn_text, pgn_bytes = _read_pgn(pgn_path)
    pgn_sha256 = hashlib.sha256(pgn_bytes).hexdigest()
    games = extract_games(pgn_text)

    if args.extract_only:
        report = build_extraction_report(
            pgn_path,
            pgn_text,
            games,
            args.movetime_ms,
            args.threads,
            pgn_sha256=pgn_sha256,
        )
    elif args.book_audit or args.book is not None:
        koi_path, stockfish_path = _resolve_engine_paths(
            args.koi, args.stockfish, "book-audit mode"
        )
        book_path = _resolve_file(args.book, "Licensed book", "--book <book.bin>")
        report = run_book_audit(
            games,
            pgn_path,
            pgn_sha256,
            koi_path,
            stockfish_path,
            book_path,
            args.movetime_ms,
            args.threads,
        )
    else:
        koi_path, stockfish_path = _resolve_engine_paths(
            args.koi, args.stockfish, "analysis mode"
        )
        report = run_search_analysis(
            games,
            pgn_path,
            pgn_sha256,
            koi_path,
            stockfish_path,
            args.movetime_ms,
            args.threads,
        )

    _write_json(args.output, report)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OracleError as error:
        print(f"elo_oracle: error: {error}", file=sys.stderr)
        raise SystemExit(2)
