#!/usr/bin/env python3
"""Run reproducible, paired-opening Koi-versus-anchor Elo measurements."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Mapping, Optional, Sequence


SCHEMA = "koi-rough-elo-estimate-v1"
ANCHOR_SCHEMA = "koi-elo-anchor-manifest-v1"
MATCH_SCHEMA = "koi-uci-match-v2"
BATCH_GAMES = 64
OPENING_COUNT = 32
BOOTSTRAP_SAMPLES = 2000
BOOTSTRAP_SEED = 20260906
STOCKFISH_MIN_ELO = 1320
STOCKFISH_MAX_ELO = 3190
TIMESTAMP_KEYS = frozenset({"timestamp", "created_at", "generated_utc", "started_at", "completed_at"})
OPENING_LINE_RE = re.compile(r"^(?P<name>[^|#\s]+)\s*\|\s*(?P<moves>[a-h][1-8][a-h][1-8][nbrq]?(?:\s+[a-h][1-8][a-h][1-8][nbrq]?)*?)\s*$")


class EloEstimateError(RuntimeError):
    """Raised for an invalid measurement input, harness result, or estimate."""


@dataclass(frozen=True)
class Opening:
    name: str
    moves: tuple[str, ...]


@dataclass(frozen=True)
class Anchor:
    id: str
    path: Path
    rating: int
    rating_source: str
    stockfish_elo: Optional[int]


@dataclass(frozen=True)
class AnchorManifest:
    path: Path
    sha256: str
    stockfish_path: Path
    anchors: tuple[Anchor, ...]


@dataclass(frozen=True)
class BatchPlan:
    ordinal: int
    anchor: Anchor
    phase: str
    games: int = BATCH_GAMES


@dataclass(frozen=True)
class MatchGame:
    opening: str
    koi_color: str
    score: float
    result: str


@dataclass(frozen=True)
class RatingFit:
    elo: Optional[int]
    raw_elo: float
    lower_bound: int
    upper_bound: int
    smoothed_scores: dict[int, float]


def _canonical(path: Path) -> Path:
    return path.expanduser().resolve()


def _same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(str(_canonical(left))) == os.path.normcase(str(_canonical(right)))


def _read_bytes(path: Path, label: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise EloEstimateError(f"{label} is missing or unreadable: {path}") from error


def _require_file(path: Path, label: str) -> Path:
    resolved = _canonical(path)
    if not resolved.is_file():
        raise EloEstimateError(f"{label} is missing: {resolved}")
    return resolved


def sha256_file(path: Path, label: str = "file") -> str:
    return hashlib.sha256(_read_bytes(_require_file(path, label), label)).hexdigest()


def parse_opening_corpus(text: str) -> tuple[Opening, ...]:
    openings: list[Opening] = []
    names: set[str] = set()
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = OPENING_LINE_RE.fullmatch(line)
        if match is None:
            raise EloEstimateError(f"invalid opening line {line_number}")
        name = match.group("name")
        if name in names:
            raise EloEstimateError(f"duplicate opening name: {name}")
        names.add(name)
        openings.append(Opening(name, tuple(match.group("moves").lower().split())))
    if len(openings) != OPENING_COUNT:
        raise EloEstimateError(f"opening corpus must contain exactly 32 openings, got {len(openings)}")
    return tuple(openings)


def load_openings(path: Path) -> tuple[Opening, ...]:
    resolved = _require_file(path, "opening corpus")
    try:
        return parse_opening_corpus(resolved.read_text(encoding="utf-8"))
    except UnicodeDecodeError as error:
        raise EloEstimateError(f"opening corpus is not UTF-8: {resolved}") from error


def _require_text(value: Any, label: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise EloEstimateError(f"anchor manifest {label} must be a non-empty string")
    return value.strip()


def _require_rating(value: Any, label: str) -> int:
    if type(value) is not int or value <= 0:
        raise EloEstimateError(f"anchor manifest {label} must be a positive integer")
    return value


def load_anchor_manifest(path: Path, stockfish_path: Path, prior_elo: int) -> AnchorManifest:
    """Load the documented anchor manifest and prove it can bracket the prior."""

    manifest_path = _require_file(path, "anchor manifest")
    requested_stockfish = _require_file(stockfish_path, "Stockfish executable")
    try:
        value = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EloEstimateError(f"invalid anchor manifest: {manifest_path}") from error
    if not isinstance(value, Mapping) or value.get("schema") != ANCHOR_SCHEMA:
        raise EloEstimateError(f"anchor manifest schema must be {ANCHOR_SCHEMA}")
    stockfish = value.get("stockfish")
    if not isinstance(stockfish, Mapping):
        raise EloEstimateError("anchor manifest stockfish object is required")
    manifest_stockfish = _require_file(Path(_require_text(stockfish.get("path"), "stockfish.path")), "manifest Stockfish executable")
    if not _same_path(manifest_stockfish, requested_stockfish):
        raise EloEstimateError("--stockfish must match anchor manifest stockfish.path")
    rating_source = _require_text(stockfish.get("rating_source"), "stockfish.rating_source")
    elos = stockfish.get("elos")
    if not isinstance(elos, list) or len(elos) < 2:
        raise EloEstimateError("anchor manifest stockfish.elos must contain at least two ratings")
    stockfish_elos = sorted({_require_rating(elo, "stockfish.elos entry") for elo in elos})
    if len(stockfish_elos) != len(elos) or any(elo < STOCKFISH_MIN_ELO or elo > STOCKFISH_MAX_ELO for elo in stockfish_elos):
        raise EloEstimateError("stockfish.elos must be unique UCI_Elo values in 1320..3190")
    anchors = [Anchor(f"stockfish-{elo}", requested_stockfish, elo, rating_source, elo) for elo in stockfish_elos]
    lower = value.get("lower_anchors", [])
    if lower is None:
        lower = []
    if not isinstance(lower, list):
        raise EloEstimateError("anchor manifest lower_anchors must be a list")
    ids = {anchor.id for anchor in anchors}
    for entry in lower:
        if not isinstance(entry, Mapping):
            raise EloEstimateError("anchor manifest lower_anchors entries must be objects")
        identifier = _require_text(entry.get("id"), "lower_anchors.id")
        rating = _require_rating(entry.get("rating"), "lower_anchors.rating")
        if identifier in ids or rating >= stockfish_elos[0]:
            raise EloEstimateError("lower anchors need unique ids and ratings below the Stockfish floor")
        ids.add(identifier)
        anchors.append(Anchor(identifier, _require_file(Path(_require_text(entry.get("path"), "lower_anchors.path")), "lower anchor executable"), rating, _require_text(entry.get("rating_source"), "lower_anchors.rating_source"), None))
    anchors.sort(key=lambda anchor: (anchor.rating, anchor.id))
    if prior_elo < stockfish_elos[0] and not any(anchor.rating <= prior_elo for anchor in anchors if anchor.stockfish_elo is None):
        raise EloEstimateError("an external lower anchor is required when the Stockfish floor cannot bracket --prior-elo")
    _initial_anchors(tuple(anchors), prior_elo)
    return AnchorManifest(manifest_path, sha256_file(manifest_path, "anchor manifest"), requested_stockfish, tuple(anchors))


def _initial_anchors(anchors: Sequence[Anchor], prior_elo: int) -> tuple[Anchor, Anchor]:
    ordered = tuple(sorted(anchors, key=lambda anchor: (anchor.rating, anchor.id)))
    if len(ordered) < 2:
        raise EloEstimateError("at least two anchors are required")
    for index, anchor in enumerate(ordered):
        if anchor.rating == prior_elo:
            if index == 0:
                return anchor, ordered[1]
            return ordered[index - 1], anchor
        if anchor.rating > prior_elo:
            if index == 0:
                break
            return ordered[index - 1], anchor
    raise EloEstimateError("anchor manifest cannot bracket --prior-elo")


def expected_score(rating: float, anchor_elo: float) -> float:
    return 1.0 / (1.0 + 10.0 ** ((anchor_elo - rating) / 400.0))


def rating_delta_for_score(score: float) -> float:
    if not 0.0 < score < 1.0:
        raise EloEstimateError("logistic score must be strictly between zero and one")
    return 400.0 * math.log10(score / (1.0 - score))


def normalize_result(result: str, koi_color: str) -> float:
    if koi_color not in {"white", "black"}:
        raise EloEstimateError("invalid Koi color")
    white_scores = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}
    if result not in white_scores:
        raise EloEstimateError("invalid or incomplete game result")
    return white_scores[result] if koi_color == "white" else 1.0 - white_scores[result]


def plan_schedule(anchors: Sequence[Anchor], prior_elo: int, target_games: int, observed_scores: Optional[Mapping[int, Sequence[float]]] = None) -> tuple[BatchPlan, ...]:
    """Plan 64-game paired batches; unobserved dry runs are deterministic."""

    if target_games not in {128, 192, 256, 320}:
        raise EloEstimateError("target games must be exactly one of 128, 192, 256, or 320")
    lower, upper = _initial_anchors(anchors, prior_elo)
    schedule = [BatchPlan(1, lower, "initial"), BatchPlan(2, upper, "initial")]
    target = float(prior_elo)
    if observed_scores:
        fit = fit_rating(observed_scores)
        target = float(fit.elo if fit.elo is not None else (fit.lower_bound + fit.upper_bound) / 2.0)
    ordered = tuple(sorted(anchors, key=lambda anchor: (abs(anchor.rating - target), anchor.rating, anchor.id)))
    while len(schedule) * BATCH_GAMES < target_games:
        schedule.append(BatchPlan(len(schedule) + 1, ordered[0], "adaptive"))
    return tuple(schedule)


def koi_option_set(mode: str, book: Optional[Path]) -> dict[str, Any]:
    options: dict[str, Any] = {"Hash": 512, "Threads": 4, "Speed": 100, "OwnBook": False}
    if mode == "no-book":
        if book is not None:
            raise EloEstimateError("--book is only valid with --mode book")
        return options
    if mode != "book" or book is None:
        raise EloEstimateError("--mode book requires --book")
    options.update({"OwnBook": True, "BookFile": str(book), "BookDepth": 16, "BookRandom": False})
    return options


def fit_rating(anchor_scores: Mapping[int, Sequence[float]]) -> RatingFit:
    """Fit a 400-point logistic model using per-anchor Jeffreys smoothing."""

    if len(anchor_scores) < 2:
        raise EloEstimateError("at least two anchors are required to fit Elo")
    rows: list[tuple[int, float, float]] = []
    smoothed: dict[int, float] = {}
    for anchor, scores in anchor_scores.items():
        if type(anchor) is not int or not scores or any(score not in {0.0, 0.5, 1.0} for score in scores):
            raise EloEstimateError("anchor scores must be non-empty chess scores")
        successes = sum(scores) + 0.5
        trials = len(scores) + 1.0
        rows.append((anchor, successes, trials))
        smoothed[anchor] = successes / trials
    rows.sort()
    lower_bound, upper_bound = rows[0][0], rows[-1][0]

    def score_derivative(rating: float) -> float:
        return sum(successes - trials * expected_score(rating, anchor) for anchor, successes, trials in rows)

    low, high = float(lower_bound - 8000), float(upper_bound + 8000)
    for _ in range(160):
        midpoint = (low + high) / 2.0
        if score_derivative(midpoint) > 0:
            low = midpoint
        else:
            high = midpoint
    raw = (low + high) / 2.0
    point = int(round(raw)) if lower_bound <= raw <= upper_bound else None
    return RatingFit(point, raw, lower_bound, upper_bound, smoothed)


def paired_bootstrap(anchor_pairs: Mapping[int, Sequence[tuple[float, float]]], *, seed: int = BOOTSTRAP_SEED, samples: int = BOOTSTRAP_SAMPLES) -> tuple[int, ...]:
    """Resample whole white/black opening pairs, never individual games."""

    if samples != BOOTSTRAP_SAMPLES or len(anchor_pairs) < 2:
        raise EloEstimateError("paired bootstrap requires exactly 2,000 samples and at least two anchors")
    normalized = {anchor: tuple(pairs) for anchor, pairs in anchor_pairs.items()}
    for anchor, pairs in normalized.items():
        if type(anchor) is not int or not pairs or len(pairs) % OPENING_COUNT != 0:
            raise EloEstimateError("paired bootstrap requires complete 32-opening pairs per anchor")
        if any(len(pair) != 2 or any(score not in {0.0, 0.5, 1.0} for score in pair) for pair in pairs):
            raise EloEstimateError("paired bootstrap contains an invalid game score")
    generator = random.Random(seed)
    estimates: list[int] = []
    for _ in range(samples):
        sampled: dict[int, list[float]] = {}
        for anchor, pairs in normalized.items():
            scores: list[float] = []
            for _ in pairs:
                scores.extend(pairs[generator.randrange(len(pairs))])
            sampled[anchor] = scores
        fit = fit_rating(sampled)
        if fit.elo is not None:
            estimates.append(fit.elo)
        else:
            estimates.append(fit.lower_bound if fit.raw_elo < fit.lower_bound else fit.upper_bound)
    return tuple(estimates)


def bootstrap_ci(samples: Sequence[int]) -> tuple[int, int]:
    if len(samples) != BOOTSTRAP_SAMPLES:
        raise EloEstimateError("bootstrap confidence intervals require 2,000 samples")
    ordered = sorted(samples)
    return ordered[49], ordered[1950]


def reproducibility_hash(value: Any) -> str:
    def stable(item: Any) -> Any:
        if isinstance(item, Mapping):
            return {str(key): stable(nested) for key, nested in item.items() if key not in TIMESTAMP_KEYS and key != "reproducibility_hash"}
        if isinstance(item, (list, tuple)):
            return [stable(nested) for nested in item]
        if isinstance(item, Path):
            return str(item)
        return item
    encoded = json.dumps(stable(value), sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def _option(name: str, value: Any) -> str:
    rendered = str(value).lower() if isinstance(value, bool) else str(value)
    return f"setoption name {name} value {rendered}"


def _engine_by_label(report: Mapping[str, Any], label: str) -> Mapping[str, Any]:
    engines = report.get("engines")
    if not isinstance(engines, list):
        raise EloEstimateError("malformed engine provenance")
    matches = [engine for engine in engines if isinstance(engine, Mapping) and engine.get("label") == label]
    if len(matches) != 1:
        raise EloEstimateError(f"malformed engine provenance for {label}")
    engine = matches[0]
    if not isinstance(engine.get("name"), str) or not engine["name"].strip() or not isinstance(engine.get("identity"), list) or not engine["identity"] or not isinstance(engine.get("options"), list):
        raise EloEstimateError(f"malformed engine provenance for {label}")
    if engine.get("process_status") != "clean shutdown" or engine.get("exit_code") != 0:
        raise EloEstimateError(f"process failure in {label} provenance")
    return engine


def _require_options(engine: Mapping[str, Any], expected: Sequence[str], label: str) -> None:
    actual = engine.get("options")
    if not isinstance(actual, list) or any(not isinstance(option, str) for option in actual):
        raise EloEstimateError(f"malformed {label} options")
    missing = [option for option in expected if option not in actual]
    if missing:
        raise EloEstimateError(f"{label} option provenance is missing: {missing[0]}")


def validate_match_manifest(report: Mapping[str, Any], expected_openings: Sequence[str], koi_color: str, anchor: Anchor, koi_options: Mapping[str, Any], time_control: Optional[str], movetime_ms: Optional[int], koi_path: Path) -> tuple[MatchGame, ...]:
    """Validate a real v2 report before its games affect an Elo estimate."""

    if not isinstance(report, Mapping) or report.get("schema") != MATCH_SCHEMA:
        raise EloEstimateError("malformed match report schema")
    configuration = report.get("configuration")
    if not isinstance(configuration, Mapping):
        raise EloEstimateError("malformed match configuration")
    if configuration.get("koi_color") != koi_color or configuration.get("games_per_position") != 1:
        raise EloEstimateError("match configuration does not match requested paired batch")
    if configuration.get("threads") != 4 or configuration.get("speed") != 100 or configuration.get("hash_mb") != 512:
        raise EloEstimateError("match configuration does not match requested Koi options")
    if time_control is not None:
        if configuration.get("time_control") != time_control or configuration.get("movetime_ms") != 0:
            raise EloEstimateError("match configuration does not match requested time control")
    elif configuration.get("movetime_ms") != movetime_ms:
        raise EloEstimateError("match configuration does not match requested movetime")
    koi_engine, opponent = _engine_by_label(report, "Koi"), _engine_by_label(report, "Opponent")
    if not _same_path(Path(str(koi_engine.get("path", ""))), koi_path) or not _same_path(Path(str(opponent.get("path", ""))), anchor.path):
        raise EloEstimateError("engine provenance path does not match requested executable")
    koi_expected = [_option(name, value) for name, value in koi_options.items()]
    koi_expected.append(_option("BookRandom", False))
    _require_options(koi_engine, koi_expected, "Koi")
    opponent_expected = [_option("Hash", 512), _option("Threads", 4), _option("Speed", 100)]
    if anchor.stockfish_elo is not None:
        opponent_expected.extend([_option("UCI_LimitStrength", True), _option("UCI_Elo", anchor.rating)])
    _require_options(opponent, opponent_expected, "Opponent")
    games = report.get("games")
    if not isinstance(games, list) or len(games) != OPENING_COUNT:
        raise EloEstimateError("paired color report must contain exactly 32 games")
    expected = set(expected_openings)
    if len(expected) != OPENING_COUNT:
        raise EloEstimateError("expected openings are malformed")
    normalized: list[MatchGame] = []
    seen: set[str] = set()
    for index, game in enumerate(games, start=1):
        if not isinstance(game, Mapping) or game.get("position") not in expected or game.get("position") in seen or game.get("koi_color") != koi_color:
            raise EloEstimateError(f"malformed game {index}")
        seen.add(str(game["position"]))
        process_status = game.get("process_status")
        if not isinstance(process_status, Mapping) or any(process_status.get(side) != "clean shutdown" for side in ("koi", "opponent")):
            raise EloEstimateError(f"process failure in game {index}")
        moves = game.get("moves")
        if not isinstance(moves, list) or not moves:
            raise EloEstimateError(f"malformed game {index}")
        if any(not isinstance(move, Mapping) or move.get("replay_legal") is not True for move in moves):
            raise EloEstimateError(f"illegal game {index}: legal replay evidence is required")
        result, termination = game.get("result"), game.get("termination")
        if not isinstance(result, str) or not isinstance(termination, str):
            raise EloEstimateError(f"malformed game {index}")
        if termination in {"illegal move", "timeout", "process exit", "max plies"}:
            raise EloEstimateError(f"incomplete game {index}: {termination}")
        if result == "*":
            if termination != "adjudicated draw":
                raise EloEstimateError(f"incomplete game {index}")
            result = "1/2-1/2"
        normalized.append(MatchGame(str(game["position"]), koi_color, normalize_result(result, koi_color), result))
    if seen != expected:
        raise EloEstimateError("paired color report is missing an expected opening")
    return tuple(normalized)


def _powershell_executable() -> str:
    for candidate in ("pwsh", "powershell", "powershell.exe"):
        if shutil.which(candidate):
            return candidate
    raise EloEstimateError("PowerShell is required to launch tools/uci_match.ps1")


def _parse_harness_paths(stdout: str) -> tuple[Path, Path]:
    json_paths, pgn_paths = [], []
    for line in stdout.splitlines():
        if line.startswith("json "):
            json_paths.append(Path(line[5:].strip()))
        elif line.startswith("pgn "):
            pgn_paths.append(Path(line[4:].strip()))
    if len(json_paths) != 1 or len(pgn_paths) != 1:
        raise EloEstimateError("match harness did not announce exactly one JSON and PGN artifact")
    return _require_file(json_paths[0], "match JSON artifact"), _require_file(pgn_paths[0], "match PGN artifact")


def _harness_command(powershell: str, koi: Path, replay: Path, anchor: Anchor, openings: Path, output_directory: Path, color: str, time_control: Optional[str], movetime_ms: Optional[int], koi_options: Mapping[str, Any]) -> list[str]:
    script = Path(__file__).with_name("uci_match.ps1").resolve()
    if not script.is_file():
        raise EloEstimateError(f"match harness is missing: {script}")
    command = [powershell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script), "-KoiPath", str(koi), "-OpponentPath", str(anchor.path), "-ReplayPath", str(replay), "-OpeningFile", str(openings), "-OutputDirectory", str(output_directory), "-Games", "1", "-KoiColor", color, "-KoiRandomSeed", "1", "-Hash", "512", "-Threads", "4", "-Speed", "100", "-KoiOwnBook", str(koi_options["OwnBook"]).lower(), "-KoiBookRandom", "false"]
    if time_control is not None:
        command.extend(["-TimeControl", time_control])
    else:
        command.extend(["-MovetimeMs", str(movetime_ms)])
    if koi_options["OwnBook"]:
        command.extend(["-KoiBookFile", str(koi_options["BookFile"]), "-KoiBookDepth", str(koi_options["BookDepth"])])
    if anchor.stockfish_elo is not None:
        command.extend(["-OpponentElo", str(anchor.rating)])
    return command


def _run_color_batch(powershell: str, koi: Path, replay: Path, anchor: Anchor, openings_path: Path, expected_openings: Sequence[str], output_directory: Path, color: str, time_control: Optional[str], movetime_ms: Optional[int], koi_options: Mapping[str, Any]) -> tuple[tuple[MatchGame, ...], list[dict[str, str]], Mapping[str, Any]]:
    command = _harness_command(powershell, koi, replay, anchor, openings_path, output_directory, color, time_control, movetime_ms, koi_options)
    try:
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as error:
        raise EloEstimateError(f"unable to launch match harness: {error}") from error
    if completed.returncode != 0:
        raise EloEstimateError(f"match harness failed for {anchor.id} Koi {color}: {completed.stderr.strip()}")
    json_path, pgn_path = _parse_harness_paths(completed.stdout)
    try:
        report = json.loads(json_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise EloEstimateError(f"invalid match JSON artifact: {json_path}") from error
    games = validate_match_manifest(report, expected_openings, color, anchor, koi_options, time_control, movetime_ms, koi)
    artifacts = [{"kind": "json", "path": str(json_path), "sha256": sha256_file(json_path, "match JSON artifact")}, {"kind": "pgn", "path": str(pgn_path), "sha256": sha256_file(pgn_path, "match PGN artifact")}]
    return games, artifacts, report


def _pairs_for_batch(white_games: Sequence[MatchGame], black_games: Sequence[MatchGame], openings: Sequence[str]) -> list[tuple[float, float]]:
    by_opening = {game.opening: game for game in white_games}
    black_by_opening = {game.opening: game for game in black_games}
    if len(by_opening) != OPENING_COUNT or len(black_by_opening) != OPENING_COUNT or set(by_opening) != set(openings) or set(black_by_opening) != set(openings):
        raise EloEstimateError("batch is not a complete 64-game paired opening measurement")
    return [(by_opening[name].score, black_by_opening[name].score) for name in openings]


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def _output_is_external(path: Path) -> Path:
    resolved = _canonical(path)
    root = Path(__file__).resolve().parents[1]
    try:
        resolved.relative_to(root)
    except ValueError:
        return resolved
    raise EloEstimateError("--output must be outside the repository")


def _hashes(manifest: AnchorManifest, koi: Path, replay: Path, stockfish: Path, openings: Path, book: Optional[Path]) -> dict[str, Any]:
    executable_hashes = {"koi": sha256_file(koi, "Koi executable"), "replay": sha256_file(replay, "replay executable"), "stockfish": sha256_file(stockfish, "Stockfish executable")}
    for anchor in manifest.anchors:
        if anchor.stockfish_elo is None:
            executable_hashes[f"lower:{anchor.id}"] = sha256_file(anchor.path, "lower anchor executable")
    return {"manifest": manifest.sha256, "executables": executable_hashes, "openings": sha256_file(openings, "opening corpus"), "book": sha256_file(book, "book file") if book is not None else None}


def _estimate_summary(games: Sequence[MatchGame], pairs: Mapping[int, Sequence[tuple[float, float]]], lower: int, upper: int) -> dict[str, Any]:
    wins = sum(game.score == 1.0 for game in games)
    draws = sum(game.score == 0.5 for game in games)
    losses = sum(game.score == 0.0 for game in games)
    results = {"games": len(games), "wins": wins, "draws": draws, "losses": losses, "score": sum(game.score for game in games) / len(games) if games else 0.0}
    if not games:
        return {"results": results, "estimate": {"elo": None, "ci95_low": None, "ci95_high": None, "half_width": None, "reliability": "not-run", "low_confidence": True, "lower_bound": lower, "upper_bound": upper}}
    by_anchor: dict[int, list[float]] = {}
    for rating, paired in pairs.items():
        by_anchor[rating] = [score for pair in paired for score in pair]
    fit = fit_rating(by_anchor)
    samples = paired_bootstrap(pairs)
    ci_low, ci_high = bootstrap_ci(samples)
    half_width = (ci_high - ci_low) / 2.0
    low_confidence = fit.elo is None or half_width > 100.0
    return {"results": results, "estimate": {"elo": fit.elo, "ci95_low": ci_low, "ci95_high": ci_high, "half_width": half_width, "reliability": "low" if low_confidence else "moderate", "low_confidence": low_confidence, "lower_bound": fit.lower_bound, "upper_bound": fit.upper_bound, "raw_logistic_elo": round(fit.raw_elo, 3), "jeffreys_smoothed_scores": {str(key): value for key, value in fit.smoothed_scores.items()}}}


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--koi", required=True, type=Path)
    parser.add_argument("--replay", required=True, type=Path)
    parser.add_argument("--stockfish", required=True, type=Path)
    parser.add_argument("--anchors", "--anchor-manifest", required=True, type=Path)
    parser.add_argument("--openings", required=True, type=Path)
    limit = parser.add_mutually_exclusive_group()
    limit.add_argument("--time-control")
    limit.add_argument("--movetime-ms", type=int)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--hash", dest="hash_mb", type=int, default=512)
    parser.add_argument("--speed", type=int, default=100)
    parser.add_argument("--min-games", type=int, default=128)
    parser.add_argument("--max-games", type=int, default=320)
    parser.add_argument("--prior-elo", required=True, type=int)
    parser.add_argument("--mode", choices=("no-book", "book"), default="no-book")
    parser.add_argument("--book", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--dry-run", action="store_true")
    return parser


def _validate_arguments(args: argparse.Namespace) -> tuple[Path, Path, Path, Path, tuple[Opening, ...], AnchorManifest, Optional[str], Optional[int], dict[str, Any], Path, dict[str, Any]]:
    if args.threads != 4 or args.hash_mb != 512 or args.speed != 100:
        raise EloEstimateError("this estimator requires --threads 4 --hash 512 --speed 100")
    if args.min_games not in {128, 192, 256, 320} or args.max_games not in {128, 192, 256, 320} or args.min_games > args.max_games:
        raise EloEstimateError("--min-games and --max-games must be 128/192/256/320 with min <= max")
    if args.movetime_ms is not None and args.movetime_ms <= 0:
        raise EloEstimateError("--movetime-ms must be positive")
    time_control = args.time_control or (None if args.movetime_ms is not None else "1+0")
    if time_control is not None and re.fullmatch(r"[1-9][0-9]*\+[0-9]+", time_control) is None:
        raise EloEstimateError("--time-control must use minutes+increment, for example 1+0")
    koi, replay, stockfish = _require_file(args.koi, "Koi executable"), _require_file(args.replay, "replay executable"), _require_file(args.stockfish, "Stockfish executable")
    openings_path, openings = _require_file(args.openings, "opening corpus"), load_openings(args.openings)
    manifest = load_anchor_manifest(args.anchors, stockfish, args.prior_elo)
    book = _require_file(args.book, "book file") if args.book is not None else None
    options = koi_option_set(args.mode, book)
    output = _output_is_external(args.output)
    return koi, replay, stockfish, openings_path, openings, manifest, time_control, args.movetime_ms, options, output, _hashes(manifest, koi, replay, stockfish, openings_path, book)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        koi, replay, stockfish, openings_path, openings, manifest, time_control, movetime_ms, options, output, hashes = _validate_arguments(args)
        dry_schedule = plan_schedule(manifest.anchors, args.prior_elo, args.max_games)
        artifacts: list[dict[str, str]] = []
        all_games: list[MatchGame] = []
        pairs: dict[int, list[tuple[float, float]]] = {}
        reports: list[dict[str, Any]] = []
        executed: list[BatchPlan] = []
        if not args.dry_run:
            powershell = _powershell_executable()
            artifact_root = output.parent / f"{output.stem}-artifacts"
            schedule = list(plan_schedule(manifest.anchors, args.prior_elo, 128))
            while schedule:
                batch = schedule.pop(0)
                batch_directory = artifact_root / f"batch-{batch.ordinal:02d}-{batch.anchor.id}"
                white, white_artifacts, white_report = _run_color_batch(powershell, koi, replay, batch.anchor, openings_path, [opening.name for opening in openings], batch_directory / "white", "white", time_control, movetime_ms, options)
                black, black_artifacts, black_report = _run_color_batch(powershell, koi, replay, batch.anchor, openings_path, [opening.name for opening in openings], batch_directory / "black", "black", time_control, movetime_ms, options)
                pairs.setdefault(batch.anchor.rating, []).extend(_pairs_for_batch(white, black, [opening.name for opening in openings]))
                all_games.extend(white)
                all_games.extend(black)
                artifacts.extend(white_artifacts + black_artifacts)
                reports.extend([{"anchor_id": batch.anchor.id, "color": "white", "engine_ids": white_report["engines"]}, {"anchor_id": batch.anchor.id, "color": "black", "engine_ids": black_report["engines"]}])
                executed.append(batch)
                if schedule:
                    continue
                if len(all_games) < args.min_games:
                    need_next = True
                else:
                    interim = _estimate_summary(all_games, pairs, manifest.anchors[0].rating, manifest.anchors[-1].rating)
                    stockfish_floor = min(anchor.rating for anchor in manifest.anchors if anchor.stockfish_elo is not None)
                    if interim["estimate"]["raw_logistic_elo"] < stockfish_floor and not any(anchor.stockfish_elo is None for anchor in manifest.anchors):
                        raise EloEstimateError("an external lower anchor is required because the fitted estimate is below the Stockfish floor")
                    need_next = bool(interim["estimate"]["low_confidence"])
                if need_next and len(all_games) < args.max_games:
                    observed = {rating: [score for pair in anchor_pairs for score in pair] for rating, anchor_pairs in pairs.items()}
                    next_schedule = plan_schedule(manifest.anchors, args.prior_elo, len(all_games) + BATCH_GAMES, observed)
                    schedule.append(next_schedule[-1])
        summary = _estimate_summary(all_games, pairs, manifest.anchors[0].rating, manifest.anchors[-1].rating)
        report: dict[str, Any] = {
            "schema": SCHEMA,
            "schema_version": 1,
            "timestamp": _utc_timestamp(),
            "estimate": summary["estimate"],
            "results": summary["results"],
            "anchors": [{"id": anchor.id, "path": str(anchor.path), "rating": anchor.rating, "rating_source": anchor.rating_source, "stockfish_uci_elo": anchor.stockfish_elo} for anchor in manifest.anchors],
            "batches": [{"ordinal": batch.ordinal, "phase": batch.phase, "anchor_id": batch.anchor.id, "anchor_rating": batch.anchor.rating, "games": batch.games} for batch in (dry_schedule if args.dry_run else executed)],
            "configuration": {"mode": args.mode, "time_control": time_control, "movetime_ms": movetime_ms, "no_book_options": koi_option_set("no-book", None), "book_options": koi_option_set("book", args.book) if args.book is not None else None, "active_koi_options": options, "engine_requirements": {"koi": {"path": str(koi), "options": options}, "stockfish": {"path": str(stockfish), "options": {"UCI_LimitStrength": True}}}, "hashes": hashes, "bootstrap": {"samples": BOOTSTRAP_SAMPLES, "seed": BOOTSTRAP_SEED, "unit": "paired opening (Koi White plus Koi Black)"}},
            "artifacts": artifacts,
            "engine_provenance": reports,
        }
        report["reproducibility_hash"] = reproducibility_hash(report)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"report {output}")
        return 0
    except (EloEstimateError, OSError, json.JSONDecodeError) as error:
        print(f"elo_estimate: error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
