"""Build deterministic, non-rating summaries from Koi bench profiles."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections import OrderedDict
from pathlib import Path
from typing import Any


class StrengthReportError(ValueError):
    """Raised when a benchmark profile cannot be compared safely."""


REQUIRED_PROFILE_FIELDS = (
    "schema",
    "engine",
    "build",
    "suite",
    "warm_hash",
    "hash_state",
    "timed",
    "hash_mb",
    "threads",
    "speed",
    "positions",
)
REQUIRED_POSITION_FIELDS = (
    "id",
    "fen",
    "limits",
    "hash_mb",
    "hash_state",
    "threads",
    "speed",
    "score_cp",
    "nodes",
    "qnodes",
    "tt_hits",
    "nps",
    "pv",
)


def _read_profile(path: Path) -> tuple[dict[str, Any], str]:
    try:
        content = path.read_bytes()
    except OSError as error:
        raise StrengthReportError(f"unable to read profile {path}: {error}") from error
    try:
        value = json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise StrengthReportError(f"invalid JSON in profile {path}: {error}") from error
    if not isinstance(value, dict):
        raise StrengthReportError(f"profile {path} must contain a JSON object")
    missing = [field for field in REQUIRED_PROFILE_FIELDS if field not in value]
    if missing:
        raise StrengthReportError(f"profile {path} is missing fields: {', '.join(missing)}")
    if value["schema"] != "koi-bench-profile-v1":
        raise StrengthReportError(f"profile {path} has unsupported schema")
    if not isinstance(value["positions"], list) or not value["positions"]:
        raise StrengthReportError(f"profile {path} must contain a non-empty positions array")
    return value, hashlib.sha256(content).hexdigest()


def _category(position_id: str) -> str:
    parts = position_id.rsplit("_", 1)
    return parts[0] if len(parts) == 2 and parts[1].isdigit() else position_id


def _number(value: Any, field: str, position_id: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise StrengthReportError(f"position {position_id} has a non-integer {field}")
    return value


def _validate_position(position: Any, path: Path) -> dict[str, Any]:
    if not isinstance(position, dict):
        raise StrengthReportError(f"profile {path} contains a non-object position")
    missing = [field for field in REQUIRED_POSITION_FIELDS if field not in position]
    if missing:
        position_id = position.get("id", "<unknown>")
        raise StrengthReportError(
            f"position {position_id} in {path} is missing fields: {', '.join(missing)}"
        )
    position_id = position["id"]
    if not isinstance(position_id, str) or not position_id:
        raise StrengthReportError(f"profile {path} contains a position with an invalid id")
    pv = position["pv"]
    if not isinstance(pv, list) or any(not isinstance(move, str) or not move for move in pv):
        raise StrengthReportError(f"position {position_id} in {path} has an invalid pv")
    for field in ("hash_mb", "threads", "speed", "score_cp", "nodes", "qnodes", "tt_hits", "nps"):
        _number(position[field], field, position_id)
    if not isinstance(position["fen"], str) or not position["fen"]:
        raise StrengthReportError(f"position {position_id} in {path} has an invalid fen")
    if not isinstance(position["limits"], dict):
        raise StrengthReportError(f"position {position_id} in {path} has invalid limits")
    if "elapsed_ms" in position:
        _number(position["elapsed_ms"], "elapsed_ms", position_id)
    if "expected_move" in position:
        if not isinstance(position["expected_move"], str) or not position["expected_move"]:
            raise StrengthReportError(f"position {position_id} in {path} has an invalid expected_move")
        accepted = position.get("accepted_moves", [position["expected_move"]])
        if not isinstance(accepted, list) or any(not isinstance(move, str) or not move for move in accepted):
            raise StrengthReportError(f"position {position_id} in {path} has invalid accepted_moves")
    return position


def _mean(values: list[int]) -> float | None:
    if not values:
        return None
    return round(sum(values) / len(values), 6)


def _profile_configuration(profile: dict[str, Any]) -> tuple[Any, ...]:
    return (
        profile["suite"],
        profile["build"],
        profile["hash_mb"],
        profile["hash_state"],
        profile["threads"],
        profile["speed"],
        profile["timed"],
        profile["warm_hash"],
    )


def build_report(paths: list[Path], expected_suite: str) -> dict[str, Any]:
    loaded = [_read_profile(path) for path in paths]
    profiles = [value for value, _ in loaded]
    hashes = [digest for _, digest in loaded]
    for path, profile in zip(paths, profiles):
        if profile["suite"] != expected_suite:
            raise StrengthReportError(
                f"profile {path} has suite {profile['suite']!r}; expected {expected_suite!r}"
            )
    configuration = _profile_configuration(profiles[0])
    if any(_profile_configuration(profile) != configuration for profile in profiles[1:]):
        raise StrengthReportError("profiles have incompatible suite, build, hash, threads, speed, or timing options")

    positions: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for path, profile in zip(paths, profiles):
        for raw_position in profile["positions"]:
            position = _validate_position(raw_position, path)
            position_id = position["id"]
            if position_id in seen_ids:
                raise StrengthReportError(f"duplicate position id: {position_id}")
            seen_ids.add(position_id)
            positions.append(position)

    category_rows: OrderedDict[str, list[dict[str, Any]]] = OrderedDict()
    for position in positions:
        category = position.get("category") or _category(position["id"])
        if not isinstance(category, str) or not category:
            raise StrengthReportError(f"position {position['id']} has an invalid category")
        category_rows.setdefault(category, []).append(position)

    mismatch_ids: list[str] = []
    expected_count = 0
    accepted_count = 0
    categories: OrderedDict[str, dict[str, Any]] = OrderedDict()
    for category, rows in category_rows.items():
        category_expected = 0
        category_accepted = 0
        category_mismatches: list[str] = []
        for position in rows:
            expected = position.get("expected_move")
            if expected is not None:
                expected_count += 1
                category_expected += 1
                accepted = position.get("accepted_moves", [expected])
                if position["pv"][0] in accepted:
                    accepted_count += 1
                    category_accepted += 1
                else:
                    mismatch_ids.append(position["id"])
                    category_mismatches.append(position["id"])
        categories[category] = {
            "position_count": len(rows),
            "expected_move_count": category_expected,
            "accepted_count": category_accepted,
            "acceptance_rate": (
                round(category_accepted / category_expected, 6) if category_expected else None
            ),
            "mean_score_cp": _mean([_number(row["score_cp"], "score_cp", row["id"]) for row in rows]),
            "mean_elapsed_ms": _mean(
                [_number(row["elapsed_ms"], "elapsed_ms", row["id"]) for row in rows if "elapsed_ms" in row]
            ),
            "total_nodes": sum(_number(row["nodes"], "nodes", row["id"]) for row in rows),
            "total_qnodes": sum(_number(row["qnodes"], "qnodes", row["id"]) for row in rows),
            "mismatch_ids": category_mismatches,
        }

    report = OrderedDict(
        (
            ("schema", "koi-strength-report-v1"),
            ("suite", expected_suite),
            ("engine", profiles[0]["engine"]),
            ("build", profiles[0]["build"]),
            ("profile_sha256", hashes if len(hashes) > 1 else hashes[0]),
            ("profile_count", len(profiles)),
            ("hash_mb", profiles[0]["hash_mb"]),
            ("hash_state", profiles[0]["hash_state"]),
            ("threads", profiles[0]["threads"]),
            ("speed", profiles[0]["speed"]),
            ("timed", profiles[0]["timed"]),
            ("position_count", len(positions)),
            ("expected_move_count", expected_count),
            ("accepted_count", accepted_count),
            ("acceptance_rate", round(accepted_count / expected_count, 6) if expected_count else None),
            ("mismatch_ids", mismatch_ids),
            ("mean_score_cp", _mean([_number(row["score_cp"], "score_cp", row["id"]) for row in positions])),
            ("mean_elapsed_ms", _mean(
                [_number(row["elapsed_ms"], "elapsed_ms", row["id"]) for row in positions if "elapsed_ms" in row]
            )),
            ("total_nodes", sum(_number(row["nodes"], "nodes", row["id"]) for row in positions)),
            ("total_qnodes", sum(_number(row["qnodes"], "qnodes", row["id"]) for row in positions)),
            ("categories", categories),
        )
    )
    return report


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", action="append", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-suite", choices=("strength", "optional_strength"), required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        report = build_report(arguments.profile, arguments.expected_suite)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (OSError, StrengthReportError) as error:
        print(f"strength_report: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
