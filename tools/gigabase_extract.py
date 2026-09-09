#!/usr/bin/env python3
"""Sample a SQLite-backed GigaBase export without modifying or copying it."""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import io
import json
import os
import random
import re
import sqlite3
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple
from urllib.parse import quote


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "koi-gigabase-manifest-v1"
DEFAULT_SEED = 240906
MAX_GAMES = 200_000
MAX_POSITIONS = 2_000_000
RESULT_TOKENS = {"1-0", "0-1", "1/2-1/2", "*"}
PGN_MOVE_NUMBER_RE = re.compile(r"^\d+\.{1,3}")
PGN_NAG_RE = re.compile(r"^\$\d+$")
SPLIT_RATIOS = {"train": 0.70, "validation": 0.15, "holdout": 0.15}
SPLIT_ORDER = ("train", "validation", "holdout")


class GigaBaseError(RuntimeError):
    """An actionable read-only GigaBase sampling error."""


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _is_inside(path: Path, directory: Path) -> bool:
    try:
        path.relative_to(directory)
    except ValueError:
        return False
    return True


def _require_external_directory(path: Path) -> Path:
    resolved = path.expanduser().resolve()
    if _is_inside(resolved, REPOSITORY_ROOT):
        raise GigaBaseError(
            f"generated reports must be outside the repository: '{resolved}'"
        )
    return resolved


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise GigaBaseError(f"unable to hash database '{path}': {error}") from error
    return digest.hexdigest()


def _resolve_database(path: Path) -> Path:
    try:
        resolved = path.expanduser().resolve(strict=True)
    except OSError as error:
        raise GigaBaseError(f"GigaBase database is missing: '{path}'") from error
    if not resolved.is_file():
        raise GigaBaseError(f"GigaBase database is not a file: '{path}'")
    return resolved


def _sqlite_read_only_uri(path: Path) -> str:
    posix_path = path.as_posix()
    if os.name == "nt":
        return f"file:///{quote(posix_path, safe='/:')}?mode=ro"
    return f"file:{quote(posix_path, safe='/')}?mode=ro"


def _open_read_only(path: Path) -> sqlite3.Connection:
    try:
        connection = sqlite3.connect(_sqlite_read_only_uri(path), uri=True)
    except sqlite3.DatabaseError as error:
        raise GigaBaseError(
            "the supplied GigaBase path is not a readable SQLite database; "
            "no database copy was created"
        ) from error
    connection.row_factory = sqlite3.Row
    try:
        connection.execute("PRAGMA query_only = ON")
        query_only = connection.execute("PRAGMA query_only").fetchone()[0]
    except sqlite3.DatabaseError as error:
        connection.close()
        raise GigaBaseError(f"unable to enable SQLite query-only mode: {error}") from error
    if int(query_only) != 1:
        connection.close()
        raise GigaBaseError("SQLite query-only mode was not enabled")
    return connection


def _quote_identifier(identifier: str) -> str:
    if not identifier or "\x00" in identifier:
        raise GigaBaseError("database identifiers must be non-empty and NUL-free")
    return '"' + identifier.replace('"', '""') + '"'


def _table_schema(connection: sqlite3.Connection) -> Dict[str, List[str]]:
    try:
        table_rows = connection.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' "
            "ORDER BY name"
        ).fetchall()
        schemas: Dict[str, List[str]] = {}
        for table_row in table_rows:
            table_name = str(table_row[0])
            columns = connection.execute(
                f"PRAGMA table_info({_quote_identifier(table_name)})"
            ).fetchall()
            schemas[table_name] = [str(column[1]) for column in columns]
        return schemas
    except sqlite3.DatabaseError as error:
        raise GigaBaseError(f"unable to inspect GigaBase schema: {error}") from error


def _find_column(
    columns: Sequence[str], requested: Optional[str], candidates: Sequence[str]
) -> Optional[str]:
    by_lower = {column.casefold(): column for column in columns}
    if requested is not None:
        return by_lower.get(requested.casefold())
    for candidate in candidates:
        if candidate.casefold() in by_lower:
            return by_lower[candidate.casefold()]
    return None


def _choose_table(
    schemas: Mapping[str, Sequence[str]], requested: Optional[str]
) -> Tuple[str, List[str]]:
    if requested is not None:
        for table_name, columns in schemas.items():
            if table_name.casefold() == requested.casefold():
                return table_name, list(columns)
        raise GigaBaseError(f"GigaBase table is missing: '{requested}'")

    table_candidates: List[Tuple[int, str, List[str]]] = []
    for table_name, columns in schemas.items():
        lowered = {column.casefold() for column in columns}
        if lowered.intersection(
            {
                "pgn",
                "movetext",
                "move_text",
                "moves",
                "notation",
                "fen",
                "position_count",
                "ply_count",
                "plies",
            }
        ):
            preference = 0 if table_name.casefold() in {"games", "game"} else 1
            table_candidates.append((preference, table_name.casefold(), list(columns)))
    if not table_candidates:
        available = ", ".join(sorted(schemas)) or "none"
        raise GigaBaseError(
            "could not identify a GigaBase games/positions table; "
            f"available tables: {available}"
        )
    _preference, sort_name, columns = sorted(table_candidates, key=lambda item: (item[0], item[1]))[0]
    table_name = next(name for name in schemas if name.casefold() == sort_name)
    return table_name, columns


def _strip_pgn_noise(pgn: str) -> str:
    output: List[str] = []
    index = 0
    variation_depth = 0
    in_brace_comment = False
    in_line_comment = False
    in_tag = False
    while index < len(pgn):
        char = pgn[index]
        if in_line_comment:
            if char in "\r\n":
                in_line_comment = False
                output.append(" ")
            index += 1
            continue
        if in_brace_comment:
            if char == "}":
                in_brace_comment = False
            index += 1
            continue
        if in_tag:
            if char == "]":
                in_tag = False
            index += 1
            continue
        if char == ";":
            in_line_comment = True
            index += 1
            continue
        if char == "{":
            in_brace_comment = True
            index += 1
            continue
        if char == "[":
            in_tag = True
            index += 1
            continue
        if char == "(":
            variation_depth += 1
            index += 1
            continue
        if char == ")":
            variation_depth = max(0, variation_depth - 1)
            index += 1
            continue
        if variation_depth == 0:
            output.append(char)
        index += 1
    return "".join(output)


def count_pgn_positions(pgn: str) -> int:
    """Count mainline move tokens without requiring a PGN runtime dependency."""

    count = 0
    for raw_token in _strip_pgn_noise(pgn).split():
        token = PGN_MOVE_NUMBER_RE.sub("", raw_token)
        if not token or token in RESULT_TOKENS or PGN_NAG_RE.fullmatch(token):
            continue
        if token in {"!", "?", "!!", "??", "!?", "?!"}:
            continue
        count += 1
    return count


def _position_count(value: Any, pgn_value: Any, fen_value: Any) -> int:
    if value is not None:
        try:
            count = int(value)
        except (TypeError, ValueError) as error:
            raise GigaBaseError(f"position count is not an integer: {value!r}") from error
        if count < 0:
            raise GigaBaseError(f"position count cannot be negative: {count}")
        return count
    if pgn_value is not None:
        return count_pgn_positions(str(pgn_value))
    if fen_value is not None:
        return 1
    return 1


def _optional_chess_modules() -> Tuple[Optional[Any], Optional[Any], Optional[str]]:
    """Load python-chess lazily so raw extraction remains standard-library-safe."""

    try:
        import chess
        import chess.pgn
    except ImportError as error:
        return None, None, str(error)
    return chess, chess.pgn, None


def _canonical_game_key(pgn_value: Any, fen_value: Any, result: str) -> str:
    """Return a move/content identity that ignores source ids and PGN tags."""

    if pgn_value is not None:
        mainline = " ".join(_strip_pgn_noise(str(pgn_value)).split()).casefold()
        payload = {"kind": "moves", "mainline": mainline, "result": result}
    elif fen_value is not None:
        payload = {"kind": "fen", "fen": " ".join(str(fen_value).split()), "result": result}
    else:
        payload = {"kind": "row", "result": result}
    return _record_hash(payload)


def _header_result(pgn_value: Any) -> Optional[str]:
    if pgn_value is None:
        return None
    for line in str(pgn_value).splitlines():
        match = re.match(r'^\s*\[Result\s+"(?P<result>[^"\s]+)"\]\s*$', line, re.IGNORECASE)
        if match and match.group("result") in RESULT_TOKENS:
            return match.group("result")
    return None


def _record_stratum(result_value: Any, pgn_value: Any) -> str:
    result = str(result_value).strip() if result_value is not None else ""
    if result not in RESULT_TOKENS:
        result = _header_result(pgn_value) or "unknown"
    return result


def _side_name(chess: Any, board: Any) -> str:
    return "white" if board.turn == chess.WHITE else "black"


def _decode_pgn(pgn_value: Any, fen_value: Any) -> Tuple[List[Dict[str, Any]], Dict[str, Any]]:
    """Decode legal mainline positions, returning a non-fatal fallback on bad input."""

    if pgn_value is None and fen_value is None:
        return [], {"status": "fallback", "reason": "no move or FEN payload"}
    chess, pgn_module, import_error = _optional_chess_modules()
    if chess is None or pgn_module is None:
        return [], {
            "status": "fallback",
            "reason": f"python-chess unavailable: {import_error or 'unknown import error'}",
        }
    try:
        if pgn_value is None:
            board = chess.Board(str(fen_value))
            return [
                {
                    "ply": 0,
                    "move_number": board.fullmove_number,
                    "side": _side_name(chess, board),
                    "fen": board.fen(),
                    "actual_move_uci": None,
                    "actual_move_san": None,
                }
            ], {"status": "decoded", "decoder": "python-chess"}

        with contextlib.redirect_stderr(io.StringIO()):
            game = pgn_module.read_game(io.StringIO(str(pgn_value)))
        if game is None:
            raise ValueError("PGN contains no game")
        if game.errors:
            raise ValueError("; ".join(str(error) for error in game.errors))
        board = game.board()
        positions: List[Dict[str, Any]] = []
        for ply, node in enumerate(game.mainline(), start=1):
            move = node.move
            if move is None or not board.is_legal(move):
                raise ValueError(f"illegal move at ply {ply}")
            positions.append(
                {
                    "ply": ply,
                    "move_number": board.fullmove_number,
                    "side": _side_name(chess, board),
                    "fen": board.fen(),
                    "actual_move_uci": move.uci(),
                    "actual_move_san": board.san(move),
                }
            )
            board.push(move)
        return positions, {"status": "decoded", "decoder": "python-chess"}
    except Exception as error:
        return [], {"status": "fallback", "reason": f"move decoding failed: {error}"}


def _record_hash(record: Mapping[str, Any]) -> str:
    canonical = json.dumps(record, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _iter_records(
    connection: sqlite3.Connection,
    table: str,
    columns: Sequence[str],
    requested_id: Optional[str] = None,
    requested_pgn: Optional[str] = None,
    requested_position_count: Optional[str] = None,
    requested_result: Optional[str] = None,
) -> Iterable[Dict[str, Any]]:
    id_column = _find_column(columns, requested_id, ("id", "game_id", "gameid", "key"))
    pgn_column = _find_column(
        columns,
        requested_pgn,
        ("pgn", "movetext", "move_text", "moves", "notation", "game"),
    )
    position_column = _find_column(
        columns,
        requested_position_count,
        ("position_count", "ply_count", "plies", "num_positions"),
    )
    result_column = _find_column(
        columns,
        requested_result,
        ("result", "outcome", "winner", "game_result"),
    )
    if requested_id is not None and id_column is None:
        raise GigaBaseError(f"table '{table}' has no id column named '{requested_id}'")
    if requested_pgn is not None and pgn_column is None:
        raise GigaBaseError(f"table '{table}' has no PGN column named '{requested_pgn}'")
    if requested_position_count is not None and position_column is None:
        raise GigaBaseError(
            f"table '{table}' has no position-count column named '{requested_position_count}'"
        )
    if requested_result is not None and result_column is None:
        raise GigaBaseError(f"table '{table}' has no result column named '{requested_result}'")
    fen_column = _find_column(columns, None, ("fen", "position", "board"))
    if id_column is None and pgn_column is None and position_column is None and fen_column is None:
        raise GigaBaseError(
            f"table '{table}' has no recognizable id, PGN, FEN, or position-count column"
        )

    selected: List[str] = []
    if id_column is not None:
        selected.append(f"{_quote_identifier(id_column)} AS __source_id")
    else:
        selected.append("rowid AS __source_id")
    for alias, column in (
        ("__pgn", pgn_column),
        ("__position_count", position_column),
        ("__fen", fen_column),
        ("__result", result_column),
    ):
        if column is not None:
            selected.append(f"{_quote_identifier(column)} AS {alias}")
    order_by = _quote_identifier(id_column) if id_column is not None else "rowid"
    query = (
        f"SELECT {', '.join(selected)} FROM {_quote_identifier(table)} "
        f"ORDER BY {order_by}"
    )
    try:
        rows = connection.execute(query)
        for row in rows:
            source_id = str(row["__source_id"])
            pgn_value = row["__pgn"] if "__pgn" in row.keys() else None
            fen_value = row["__fen"] if "__fen" in row.keys() else None
            position_value = row["__position_count"] if "__position_count" in row.keys() else None
            result_value = row["__result"] if "__result" in row.keys() else None
            position_count = _position_count(position_value, pgn_value, fen_value)
            stratum = _record_stratum(result_value, pgn_value)
            record: Dict[str, Any] = {
                "source_id": source_id,
                "position_count": position_count,
                "legal_position_count": 0,
                "positions": [],
                "stratum": stratum,
                "deduplication_key": _canonical_game_key(pgn_value, fen_value, stratum),
                "decode": {"status": "pending"},
                "_pgn_value": pgn_value,
                "_fen_value": fen_value,
            }
            if pgn_value is not None:
                record["pgn_sha256"] = hashlib.sha256(
                    str(pgn_value).encode("utf-8")
                ).hexdigest()
            if fen_value is not None:
                record["fen_sha256"] = hashlib.sha256(
                    str(fen_value).encode("utf-8")
                ).hexdigest()
            record["record_sha256"] = _record_hash(record)
            yield record
    except sqlite3.DatabaseError as error:
        raise GigaBaseError(f"unable to read GigaBase table '{table}': {error}") from error


def _sample_records(
    records: Iterable[Dict[str, Any]],
    seed: int,
    max_games: int,
    max_positions: int,
) -> Tuple[List[Dict[str, Any]], int, int]:
    reservoirs: Dict[str, List[Dict[str, Any]]] = {}
    stratum_counts: Dict[str, int] = {}
    stratum_rngs: Dict[str, random.Random] = {}
    seen_keys: set[str] = set()
    scanned = 0
    unique = 0
    for record in records:
        scanned += 1
        deduplication_key = str(record["deduplication_key"])
        if deduplication_key in seen_keys:
            continue
        seen_keys.add(deduplication_key)
        stratum = str(record["stratum"])
        if stratum not in reservoirs:
            reservoirs[stratum] = []
            stratum_counts[stratum] = 0
            stable_seed = int.from_bytes(
                hashlib.sha256(stratum.encode("utf-8")).digest()[:8], "big"
            )
            stratum_rngs[stratum] = random.Random(seed ^ stable_seed)
        stratum_counts[stratum] += 1
        unique += 1
        reservoir = reservoirs[stratum]
        if len(reservoir) < max_games:
            reservoir.append(record)
            continue
        replacement = stratum_rngs[stratum].randrange(stratum_counts[stratum])
        if replacement < max_games:
            reservoir[replacement] = record

    target = min(max_games, unique)
    candidate_records: List[Dict[str, Any]] = []
    if target > 0:
        allocations = _largest_remainder_allocation(stratum_counts, target)
        for stratum in sorted(reservoirs):
            reservoir = reservoirs[stratum]
            stratum_rngs[stratum].shuffle(reservoir)
            candidate_records.extend(reservoir[: allocations.get(stratum, 0)])

    random.Random(seed).shuffle(candidate_records)
    selected: List[Dict[str, Any]] = []
    positions = 0
    for record in candidate_records:
        record_positions = int(record["position_count"])
        if positions + record_positions > max_positions:
            continue
        selected.append(record)
        positions += record_positions
        if len(selected) >= max_games:
            break
    return selected, scanned, unique


def _largest_remainder_allocation(counts: Mapping[str, int], target: int) -> Dict[str, int]:
    if target <= 0 or not counts:
        return {stratum: 0 for stratum in counts}
    total = sum(counts.values())
    if total <= 0:
        return {stratum: 0 for stratum in counts}
    raw = {stratum: target * count / total for stratum, count in counts.items()}
    allocation = {stratum: int(value) for stratum, value in raw.items()}
    remaining = target - sum(allocation.values())
    order = sorted(
        counts,
        key=lambda stratum: (-(raw[stratum] - allocation[stratum]), str(stratum)),
    )
    for stratum in order[:remaining]:
        allocation[stratum] += 1
    return allocation


def _split_counts(count: int) -> Dict[str, int]:
    if count <= 0:
        return {split: 0 for split in SPLIT_ORDER}
    raw = {split: count * SPLIT_RATIOS[split] for split in SPLIT_ORDER}
    counts = {split: int(raw[split]) for split in SPLIT_ORDER}
    remaining = count - sum(counts.values())
    order = sorted(
        SPLIT_ORDER,
        key=lambda split: (-(raw[split] - counts[split]), SPLIT_ORDER.index(split)),
    )
    for split in order[:remaining]:
        counts[split] += 1
    return counts


def _split_records(records: Sequence[Dict[str, Any]]) -> Dict[str, List[Dict[str, Any]]]:
    allocations = _split_counts(len(records))
    by_stratum: Dict[str, List[Dict[str, Any]]] = {}
    for record in records:
        by_stratum.setdefault(str(record["stratum"]), []).append(record)

    # Interleave strata before slicing exact global quotas. This keeps common
    # result/outcome strata represented in every split when the sample permits.
    interleaved: List[Dict[str, Any]] = []
    offsets = {stratum: 0 for stratum in by_stratum}
    while True:
        progressed = False
        for stratum in sorted(by_stratum):
            offset = offsets[stratum]
            if offset < len(by_stratum[stratum]):
                interleaved.append(by_stratum[stratum][offset])
                offsets[stratum] = offset + 1
                progressed = True
        if not progressed:
            break

    train_end = allocations["train"]
    validation_end = train_end + allocations["validation"]
    return {
        "train": interleaved[:train_end],
        "validation": interleaved[train_end:validation_end],
        "holdout": interleaved[validation_end:],
    }


def _content_hash(value: Mapping[str, Any]) -> str:
    canonical = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _decode_selected_record(record: Dict[str, Any]) -> Dict[str, Any]:
    positions, decode = _decode_pgn(record.get("_pgn_value"), record.get("_fen_value"))
    record["positions"] = positions
    record["legal_position_count"] = len(positions)
    record["decode"] = decode
    if decode.get("status") == "decoded":
        record["position_count"] = len(positions)
    record.pop("_pgn_value", None)
    record.pop("_fen_value", None)
    record["record_sha256"] = _record_hash(
        {key: value for key, value in record.items() if key != "record_sha256"}
    )
    return record


def sample_database(
    database: Path,
    seed: int = DEFAULT_SEED,
    max_games: int = MAX_GAMES,
    max_positions: int = MAX_POSITIONS,
    table: Optional[str] = None,
    id_column: Optional[str] = None,
    pgn_column: Optional[str] = None,
    position_count_column: Optional[str] = None,
    result_column: Optional[str] = None,
) -> Dict[str, Any]:
    """Return a bounded deterministic sample and its train/validation/holdout splits."""

    if max_games <= 0 or max_games > MAX_GAMES:
        raise GigaBaseError(f"max games must be in the range 1..{MAX_GAMES}")
    if max_positions <= 0 or max_positions > MAX_POSITIONS:
        raise GigaBaseError(f"max positions must be in the range 1..{MAX_POSITIONS}")
    resolved_database = _resolve_database(database)
    database_hash = sha256_file(resolved_database)
    connection = _open_read_only(resolved_database)
    try:
        schemas = _table_schema(connection)
        selected_table, columns = _choose_table(schemas, table)
        selected_records, scanned_games, deduplicated_games = _sample_records(
            _iter_records(
                connection,
                selected_table,
                columns,
                requested_id=id_column,
                requested_pgn=pgn_column,
                requested_position_count=position_count_column,
                requested_result=result_column,
            ),
            seed,
            max_games,
            max_positions,
        )
        query_only = int(connection.execute("PRAGMA query_only").fetchone()[0]) == 1
    finally:
        connection.close()

    source = {
        "database_path": str(resolved_database),
        "database_sha256": database_hash,
        "table": selected_table,
        "read_only": True,
        "query_only": query_only,
        "decoder": "python-chess when available; bounded raw-record fallback otherwise",
    }
    decoded_candidates = [_decode_selected_record(record) for record in selected_records]
    legal_positions = 0
    selected_records = []
    for record in decoded_candidates:
        record_legal_positions = int(record["legal_position_count"])
        if legal_positions + record_legal_positions > max_positions:
            continue
        selected_records.append(record)
        legal_positions += record_legal_positions
    splits = _split_records(selected_records)
    split_counts = {split: len(records) for split, records in splits.items()}
    sampling_content = {
        "seed": int(seed),
        "max_games": max_games,
        "max_positions": max_positions,
        "scanned_games": scanned_games,
        "deduplicated_games": deduplicated_games,
        "selected_games": len(selected_records),
        "selected_positions": sum(int(record["position_count"]) for record in selected_records),
        "selected_legal_positions": sum(
            int(record["legal_position_count"]) for record in selected_records
        ),
        "split_ratios": dict(SPLIT_RATIOS),
        "split_counts": split_counts,
        "stratification": {
            "field": "result/outcome column, then PGN Result header, then unknown",
            "strata": sorted({str(record["stratum"]) for record in selected_records}),
        },
    }
    sampling_content["content_sha256"] = _content_hash(
        {"records": selected_records, "splits": splits, "sampling": sampling_content}
    )
    return {
        "schema": SCHEMA,
        "schema_version": 1,
        "created_at": _utc_timestamp(),
        "source": source,
        "sampling": sampling_content,
        "records": selected_records,
        "splits": splits,
    }


def _manifest_for_split(
    split: str,
    sample: Mapping[str, Any],
    records: Sequence[Dict[str, Any]],
) -> Dict[str, Any]:
    source = dict(sample["source"])
    sampling = dict(sample["sampling"])
    content = {
        "schema": SCHEMA,
        "schema_version": 1,
        "split": split,
        "source": {
            "database_sha256": source["database_sha256"],
            "table": source["table"],
        },
        "sampling": sampling,
        "records": list(records),
    }
    return {
        **content,
        "created_at": sample["created_at"],
        "content_sha256": _content_hash(content),
        "source": source,
        "record_count": len(records),
        "position_count": sum(int(record["position_count"]) for record in records),
        "legal_position_count": sum(
            int(record["legal_position_count"]) for record in records
        ),
    }


def _summary_for_sample(sample: Mapping[str, Any], manifests: Mapping[str, Mapping[str, Any]]) -> Dict[str, Any]:
    split_summary = {
        split: {
            "record_count": int(manifest["record_count"]),
            "position_count": int(manifest["position_count"]),
            "content_sha256": manifest["content_sha256"],
        }
        for split, manifest in manifests.items()
    }
    content = {
        "schema": SCHEMA,
        "schema_version": 1,
        "source": {
            "database_sha256": sample["source"]["database_sha256"],
            "table": sample["source"]["table"],
        },
        "sampling": sample["sampling"],
        "splits": split_summary,
    }
    return {
        "schema": SCHEMA,
        "schema_version": 1,
        "created_at": sample["created_at"],
        "source": sample["source"],
        "sampling": sample["sampling"],
        "splits": split_summary,
        "content_sha256": _content_hash(content),
    }


def write_manifests(sample: Mapping[str, Any], output_directory: Path) -> Dict[str, Path]:
    output = _require_external_directory(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    manifests = {
        split: _manifest_for_split(split, sample, records)
        for split, records in sample["splits"].items()
    }
    paths: Dict[str, Path] = {}
    try:
        for split, manifest in manifests.items():
            path = output / f"{split}.json"
            path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            paths[split] = path
        summary_path = output / "summary.json"
        summary_path.write_text(
            json.dumps(_summary_for_sample(sample, manifests), indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        paths["summary"] = summary_path
    except OSError as error:
        raise GigaBaseError(f"unable to write GigaBase manifests to '{output}': {error}") from error
    return paths


def _positive_bounded(value: str, maximum: int) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed <= 0 or parsed > maximum:
        raise argparse.ArgumentTypeError(f"must be in the range 1..{maximum}")
    return parsed


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", required=True, type=Path, help="SQLite-backed GigaBase path")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "koi-results" / "gigabase",
        help="external output directory for split manifests",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument(
        "--max-games",
        type=lambda value: _positive_bounded(value, MAX_GAMES),
        default=MAX_GAMES,
    )
    parser.add_argument(
        "--max-positions",
        type=lambda value: _positive_bounded(value, MAX_POSITIONS),
        default=MAX_POSITIONS,
    )
    parser.add_argument("--table", help="database table (default: schema-based discovery)")
    parser.add_argument("--id-column", help="game identifier column")
    parser.add_argument("--pgn-column", help="PGN/movetext column")
    parser.add_argument("--position-count-column", help="precomputed position-count column")
    parser.add_argument("--result-column", help="result/outcome column used for stratification")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    _require_external_directory(args.output_dir)
    sample = sample_database(
        args.database,
        seed=args.seed,
        max_games=args.max_games,
        max_positions=args.max_positions,
        table=args.table,
        id_column=args.id_column,
        pgn_column=args.pgn_column,
        position_count_column=args.position_count_column,
        result_column=args.result_column,
    )
    paths = write_manifests(sample, args.output_dir)
    for name in ("train", "validation", "holdout", "summary"):
        print(f"{name} {paths[name]}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GigaBaseError as error:
        print(f"gigabase_extract: error: {error}", file=sys.stderr)
        raise SystemExit(2)
