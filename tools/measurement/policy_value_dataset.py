"""Versioned, dependency-free JSONL contract for Koi policy/value examples.

The record keeps the producer's deterministic legal-action order. Policy and
visit arrays use that same order; this module validates alignment but does not
generate chess legality or reorder the actions.
"""

from __future__ import annotations

import json
import math
import os
import re
import tempfile
from pathlib import Path
from typing import Any, Iterable, Iterator


SCHEMA = "koi-policy-value-dataset-v1"
ACTION_ENCODING = "koi-uci-action-v1"
OUTCOMES = {"win", "draw", "loss"}
PROMOTIONS = {"q", "r", "b", "n"}
_ACTION_RE = re.compile(r"^([a-h][1-8])([a-h][1-8])([qrbn])?$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_RECORD_FIELDS = {
    "schema",
    "action_encoding",
    "position",
    "legal_actions",
    "policy_targets",
    "value_target",
    "outcome",
    "opening_id",
    "game_id",
    "seed",
    "ply",
    "feature_schema",
    "producer",
    "search_provenance",
    "network_sha256",
    "termination_reason",
}
_POSITION_FIELDS = {"fen", "variant"}
_ACTION_FIELDS = {"uci", "from", "to", "promotion"}
_PRODUCER_FIELDS = {"kind", "name", "version"}
_SEARCH_FIELDS = {"algorithm", "options"}


class DatasetError(ValueError):
    """Raised when a policy/value record or JSONL stream violates the contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise DatasetError(message)


def _nonempty_string(value: Any, field: str) -> None:
    _require(isinstance(value, str) and bool(value.strip()), f"{field} must be a non-empty string")


def _finite_number(value: Any) -> bool:
    return isinstance(value, int) or (isinstance(value, float) and math.isfinite(value))


def _fen_integer_at_least(value: str, minimum: int) -> bool:
    if re.fullmatch(r"[0-9]+", value) is None:
        return False
    try:
        return int(value) >= minimum
    except (ValueError, OverflowError):
        return False


def _json_value(value: Any, field: str) -> None:
    """Reject values JSON can silently coerce or encode non-portably."""
    if value is None or isinstance(value, (str, bool, int)):
        return
    if isinstance(value, float):
        _require(math.isfinite(value), f"{field} must not contain NaN or infinity")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _json_value(item, f"{field}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            _require(isinstance(key, str), f"{field} keys must be strings")
            _json_value(item, f"{field}.{key}")
        return
    raise DatasetError(f"{field} contains a non-JSON value")


def validate_record(record: Any) -> None:
    """Validate one v1 record. Raises :class:`DatasetError` on any violation."""
    _require(isinstance(record, dict), "record must be a JSON object")
    fields = set(record)
    _require(fields in (_RECORD_FIELDS, _RECORD_FIELDS | {"visit_counts"}), "record fields do not match schema v1")
    _require(record["schema"] == SCHEMA, f"schema must be {SCHEMA}")
    _require(record["action_encoding"] == ACTION_ENCODING, f"action_encoding must be {ACTION_ENCODING}")

    position = record["position"]
    _require(isinstance(position, dict) and set(position) == _POSITION_FIELDS, "position must contain exactly fen and variant")
    fen = position["fen"]
    _nonempty_string(fen, "position.fen")
    fen_fields = fen.split()
    _require(len(fen_fields) == 6, "position.fen must contain all six FEN fields")
    _require(fen_fields[1] in {"w", "b"}, "position.fen has an invalid active color")
    _require(_fen_integer_at_least(fen_fields[4], 0), "position.fen has an invalid halfmove clock")
    _require(_fen_integer_at_least(fen_fields[5], 1), "position.fen has an invalid fullmove number")
    _nonempty_string(position["variant"], "position.variant")

    actions = record["legal_actions"]
    _require(isinstance(actions, list) and bool(actions), "legal_actions must be a non-empty ordered list")
    seen_actions: set[str] = set()
    for index, action in enumerate(actions):
        label = f"legal_actions[{index}]"
        _require(isinstance(action, dict) and set(action) == _ACTION_FIELDS, f"{label} must contain exactly uci, from, to, and promotion")
        for component in ("uci", "from", "to"):
            _require(isinstance(action[component], str), f"{label}.{component} must be a string")
        match = _ACTION_RE.fullmatch(action["uci"])
        _require(match is not None, f"{label}.uci is not a valid lowercase UCI action")
        source, target, encoded_promotion = match.groups()
        _require(source != target, f"{label}.uci cannot move a square to itself")
        _require(action["from"] == source and action["to"] == target, f"{label} UCI and from/to fields disagree")
        promotion = action["promotion"]
        _require(promotion is None or (isinstance(promotion, str) and promotion in PROMOTIONS), f"{label}.promotion must be null or q, r, b, n")
        _require(promotion == encoded_promotion, f"{label}.promotion and UCI suffix disagree")
        _require(action["uci"] not in seen_actions, f"{label}.uci duplicates an earlier legal action")
        seen_actions.add(action["uci"])

    targets = record["policy_targets"]
    _require(isinstance(targets, list) and len(targets) == len(actions), "policy_targets must align with legal_actions")
    for index, target in enumerate(targets):
        _require(isinstance(target, (int, float)) and not isinstance(target, bool), f"policy_targets[{index}] must be a number")
        _require(_finite_number(target) and 0.0 <= target <= 1.0, f"policy_targets[{index}] must be finite and in [0, 1]")
    _require(math.isclose(sum(targets), 1.0, rel_tol=0.0, abs_tol=1e-9), "policy_targets must be normalized to sum to one")

    if "visit_counts" in record:
        counts = record["visit_counts"]
        _require(isinstance(counts, list) and len(counts) == len(actions), "visit_counts must align with legal_actions")
        for index, count in enumerate(counts):
            _require(isinstance(count, int) and not isinstance(count, bool) and count >= 0, f"visit_counts[{index}] must be a non-negative integer")

    value = record["value_target"]
    _require(isinstance(value, (int, float)) and not isinstance(value, bool), "value_target must be a number")
    _require(_finite_number(value) and -1.0 <= value <= 1.0, "value_target must be finite and in [-1, 1]")
    outcome = record["outcome"]
    _require(isinstance(outcome, str) and outcome in OUTCOMES, "outcome must be win, draw, or loss from the side-to-move perspective")

    for field in ("opening_id", "game_id", "feature_schema", "termination_reason"):
        _nonempty_string(record[field], field)
    for field in ("seed", "ply"):
        _require(isinstance(record[field], int) and not isinstance(record[field], bool) and record[field] >= 0, f"{field} must be a non-negative integer")

    producer = record["producer"]
    _require(isinstance(producer, dict) and set(producer) == _PRODUCER_FIELDS, "producer must contain exactly kind, name, and version")
    for field in _PRODUCER_FIELDS:
        _nonempty_string(producer[field], f"producer.{field}")

    search = record["search_provenance"]
    _require(isinstance(search, dict) and set(search) == _SEARCH_FIELDS, "search_provenance must contain exactly algorithm and options")
    _nonempty_string(search["algorithm"], "search_provenance.algorithm")
    _require(isinstance(search["options"], dict), "search_provenance.options must be an object")
    _json_value(search["options"], "search_provenance.options")

    network_hash = record["network_sha256"]
    if network_hash is None:
        _require(producer["kind"] == "alpha-beta-distillation", "network_sha256 may be null only for alpha-beta distillation")
    else:
        _require(isinstance(network_hash, str) and _SHA256_RE.fullmatch(network_hash) is not None, "network_sha256 must be a lowercase SHA-256 hex digest")


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DatasetError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> None:
    raise DatasetError(f"non-finite JSON number is not allowed: {value}")


def read_jsonl(path: str | os.PathLike[str]) -> Iterator[dict[str, Any]]:
    """Yield validated records from a strict UTF-8 JSONL file."""
    try:
        stream = Path(path).open("rb")
    except OSError as error:
        raise DatasetError(f"cannot open dataset: {error}") from error
    with stream:
        for line_number, raw_line in enumerate(stream, 1):
            try:
                line = raw_line.decode("utf-8")
            except UnicodeError as error:
                raise DatasetError(f"line {line_number}: {error}") from error
            if not line.strip():
                raise DatasetError(f"line {line_number}: blank lines are not valid JSONL records")
            try:
                record = json.loads(
                    line,
                    object_pairs_hook=_reject_duplicate_keys,
                    parse_constant=_reject_constant,
                )
                validate_record(record)
            except (ValueError, OverflowError) as error:
                raise DatasetError(f"line {line_number}: {error}") from error
            yield record


def write_jsonl(path: str | os.PathLike[str], records: Iterable[dict[str, Any]]) -> None:
    """Atomically write validated records in deterministic canonical JSON form."""
    destination = Path(path)
    temporary_path: Path | None = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=destination.parent,
            prefix=f".{destination.name}.", suffix=".tmp", delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            for record in records:
                validate_record(record)
                encoded = json.dumps(
                    record,
                    ensure_ascii=False,
                    allow_nan=False,
                    sort_keys=True,
                    separators=(",", ":"),
                )
                stream.write(encoded)
                stream.write("\n")
        os.replace(temporary_path, destination)
    except BaseException as error:
        if temporary_path is not None:
            try:
                temporary_path.unlink(missing_ok=True)
            except OSError:
                pass
        if isinstance(error, (OSError, TypeError, ValueError)) and not isinstance(error, DatasetError):
            raise DatasetError(f"cannot write dataset: {error}") from error
        raise

