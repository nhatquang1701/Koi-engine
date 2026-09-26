#!/usr/bin/env python3
"""Assess a color-reversed Koi release match against a frozen baseline.

The release decision uses a one-sided exact Clopper-Pearson upper bound on the
probability that a paired-opening score is below 0.5. Paired-opening score is
the average of candidate scores in the white and black games, so the two games
may be arbitrarily dependent. Given the upper bound p_U, mean score is at
least 0.5*(1-p_U), since all non-adverse pairs score at least 0.5 and adverse
pairs score at least zero. This conservatively ignores favorable scores. The
binomial coverage assumes opening pairs were selected before outcomes as IID
draws from the target distribution: a curated root uniformly at random, then
a legal one-ply continuation uniformly at random. Sampling is with replacement.
Results generalize to that declared opening distribution.

The Elo conversion assumes the conventional logistic 400*log10(p/(1-p))
relation. A separate Hoeffding lower bound over pair scores is reported as a
diagnostic only; it does not drive the pass decision. Games ending at the
harness ply cap (PGN result '*', termination 'max plies') are explicitly
counted as draws, a conservative full-game score of 0.5.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "measurement"))
import koi_chess as chess  # noqa: E402


class StrengthBoundError(ValueError):
    """Input evidence is incomplete or incompatible with the requested gate."""


RESULTS = {"1-0", "0-1", "1/2-1/2", "*"}
ENGINE_LABELS = {"Koi": "candidate", "Opponent": "baseline"}


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise StrengthBoundError(f"cannot read match JSON {path}: {error}") from error
    if not isinstance(value, dict):
        raise StrengthBoundError(f"match JSON {path} must contain an object")
    if value.get("schema") != "koi-uci-match-v2":
        raise StrengthBoundError(f"match JSON {path} has unsupported schema")
    return value


def _sha256(match: dict[str, Any], label: str, path: Path) -> str:
    engines = match.get("engines")
    if not isinstance(engines, list):
        raise StrengthBoundError(f"{path} has no engine evidence")
    found = [engine for engine in engines if isinstance(engine, dict) and engine.get("label") == label]
    if len(found) != 1:
        raise StrengthBoundError(f"{path} must contain exactly one {label} engine")
    hashes = found[0].get("hashes")
    digest = hashes.get("executable_sha256") if isinstance(hashes, dict) else None
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise StrengthBoundError(f"{path} has invalid {label} executable SHA-256")
    if found[0].get("process_status") != "clean shutdown" or found[0].get("exit_code") != 0:
        raise StrengthBoundError(f"{path} {label} engine did not shut down cleanly")
    return digest.lower()


def _validate_config(match: dict[str, Any], expected: dict[str, Any], color: str, path: Path) -> dict[str, Any]:
    config = match.get("configuration")
    if not isinstance(config, dict):
        raise StrengthBoundError(f"{path} has no configuration object")
    if config.get("koi_color") != color:
        raise StrengthBoundError(f"{path} koi_color must be {color}")
    for key, value in expected.items():
        if config.get(key) != value:
            raise StrengthBoundError(f"{path} configuration {key}={config.get(key)!r}; expected {value!r}")
    if config.get("koi_own_book") is not False or config.get("koi_book_random") is not False:
        raise StrengthBoundError(f"{path} must have Koi book use disabled and deterministic")
    if config.get("opponent_limit_strength") is not False:
        raise StrengthBoundError(f"{path} must have opponent strength limiting disabled")
    measurement = match.get("measurement", {})
    if not isinstance(measurement, dict):
        raise StrengthBoundError(f"{path} has invalid measurement metadata")
    book = measurement.get("book", {})
    tablebase = measurement.get("tablebase", {})
    if (not isinstance(book, dict) or book.get("enabled") is not False or
            not isinstance(tablebase, dict) or tablebase.get("enabled") is not False):
        raise StrengthBoundError(f"{path} must record books and tablebases disabled")
    return {key: value for key, value in config.items() if key != "koi_color"}


RELEVANT_UCI_OPTIONS = {
    "RandomSeed", "Hash", "Threads", "Speed", "OwnBook", "BookFile",
    "BookDepth", "BookRandom", "SyzygyPath", "SyzygyProbeDepth",
    "SyzygyProbeLimit", "SyzygyInteriorDepth", "Syzygy50MoveRule",
    "EvalFile", "StrengthMode",
}


def _effective_uci_options(engine: dict[str, Any], label: str, path: Path) -> dict[str, str]:
    handshake = engine.get("handshake")
    sent_options = engine.get("options")
    if not isinstance(handshake, list) or not isinstance(sent_options, list):
        raise StrengthBoundError(f"{path} {label} engine is missing UCI option evidence")
    effective: dict[str, str] = {}
    for line in handshake:
        if not isinstance(line, str):
            continue
        match = re.match(r"^option name (.+?) type \S+ default (.*?)(?: min \S+| max \S+|$)", line)
        if match and match.group(1) in RELEVANT_UCI_OPTIONS:
            effective[match.group(1)] = match.group(2).strip()
    for line in sent_options:
        if not isinstance(line, str):
            raise StrengthBoundError(f"{path} {label} has an invalid UCI option record")
        match = re.match(r"^setoption name (.+?)(?: value (.*))?$", line)
        if match and match.group(1) in RELEVANT_UCI_OPTIONS:
            if match.group(2) is None:
                raise StrengthBoundError(f"{path} {label} UCI option {match.group(1)} lacks a value")
            effective[match.group(1)] = match.group(2).strip()
    return effective


def _validate_engine_options(match: dict[str, Any], path: Path) -> None:
    engines = match["engines"]
    koi = next(engine for engine in engines if engine.get("label") == "Koi")
    opponent = next(engine for engine in engines if engine.get("label") == "Opponent")
    candidate_options = _effective_uci_options(koi, "candidate", path)
    baseline_options = _effective_uci_options(opponent, "baseline", path)
    for name in sorted(RELEVANT_UCI_OPTIONS):
        candidate_value = candidate_options.get(name)
        baseline_value = baseline_options.get(name)
        if candidate_value != baseline_value:
            raise StrengthBoundError(
                f"{path} candidate/baseline UCI setting {name} differs "
                f"({candidate_value!r} versus {baseline_value!r})"
            )


def _pgn_games(path: Path) -> list[tuple[str, list[str]]]:
    try:
        content = path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeError) as error:
        raise StrengthBoundError(f"cannot read PGN {path}: {error}") from error
    blocks = [block for block in re.split(r'(?m)(?=^\[Event\s+")', content.strip()) if block.strip()]
    if not blocks:
        raise StrengthBoundError(f"{path} has no PGN games")
    games: list[tuple[str, list[str]]] = []
    for index, block in enumerate(blocks, 1):
        lines = block.splitlines()
        separator = next((number for number, line in enumerate(lines) if not line.strip()), None)
        if separator is None or not lines[0].startswith('[Event "'):
            raise StrengthBoundError(f"{path} PGN game {index} has no header/movetext separator")
        headers = lines[:separator]
        result_headers = [match.group(1) for line in headers
                          if (match := re.fullmatch(r'\[Result\s+"([^"]+)"\]', line))]
        if len(result_headers) != 1 or result_headers[0] not in RESULTS:
            raise StrengthBoundError(f"{path} PGN game {index} has invalid result header")
        if '[MoveFormat "UCI coordinate notation"]' not in headers:
            raise StrengthBoundError(f"{path} PGN game {index} lacks UCI coordinate move format")
        tokens = " ".join(lines[separator + 1:]).split()
        if not tokens or tokens[-1] != result_headers[0]:
            raise StrengthBoundError(f"{path} PGN game {index} has missing or mismatched movetext result")
        moves: list[str] = []
        for token in tokens[:-1]:
            if re.fullmatch(r'\d+\.(?:\.\.)?', token):
                continue
            if not re.fullmatch(r'[a-h][1-8][a-h][1-8][nbrq]?', token):
                raise StrengthBoundError(f"{path} PGN game {index} has invalid move token {token!r}")
            moves.append(token)
        if not moves:
            raise StrengthBoundError(f"{path} PGN game {index} has no moves")
        games.append((result_headers[0], moves))
    return games


def _game_result(game: Any, color: str, path: Path, index: int) -> tuple[float, bool]:
    if not isinstance(game, dict):
        raise StrengthBoundError(f"{path} game {index + 1} is not an object")
    termination = game.get("termination")
    result = game.get("result")
    if result not in RESULTS:
        raise StrengthBoundError(f"{path} game {index + 1} has invalid result {result!r}")
    status = game.get("process_status")
    if not isinstance(status, dict) or status.get("koi") != "clean shutdown" or status.get("opponent") != "clean shutdown":
        raise StrengthBoundError(f"{path} game {index + 1} has an unclean engine process")
    moves = game.get("moves")
    if not isinstance(moves, list) or not moves or any(
        not isinstance(move, dict) or move.get("replay_legal") is not True
        or not isinstance(move.get("move"), str)
        for move in moves
    ):
        raise StrengthBoundError(f"{path} game {index + 1} has missing or illegal moves")
    if result == "*":
        if termination != "max plies":
            raise StrengthBoundError(f"{path} game {index + 1} is unfinished without a max-plies draw")
        return 0.5, True
    if termination == "max plies":
        raise StrengthBoundError(f"{path} game {index + 1} max-plies result must be '*'")
    if result == "1/2-1/2":
        return 0.5, False
    candidate_is_white = color == "white"
    candidate_won = (result == "1-0") == candidate_is_white
    return (1.0 if candidate_won else 0.0), False


def _load_side(path: Path, color: str, expected: dict[str, Any], candidate_hash: str,
               baseline_hash: str) -> tuple[dict[str, float], dict[str, bool], dict[str, Any], dict[str, tuple[Any, Any]]]:
    match = _read_json(path)
    normalized = _validate_config(match, expected, color, path)
    for label, expected_hash in (("Koi", candidate_hash), ("Opponent", baseline_hash)):
        observed = _sha256(match, label, path)
        if observed != expected_hash:
            role = ENGINE_LABELS[label]
            raise StrengthBoundError(f"{path} {role} SHA-256 mismatch: observed {observed}, expected {expected_hash}")
    _validate_engine_options(match, path)
    positions = match.get("positions")
    games = match.get("games")
    if not isinstance(positions, list) or not positions or not isinstance(games, list) or len(games) != len(positions):
        raise StrengthBoundError(f"{path} must have one recorded game per opening position")
    names = [p.get("Name") if isinstance(p, dict) else None for p in positions]
    if any(not isinstance(name, str) or not name for name in names) or len(names) != len(set(names)):
        raise StrengthBoundError(f"{path} contains missing or duplicate opening names")
    opening_specs: dict[str, tuple[Any, Any]] = {}
    for position in positions:
        if not isinstance(position.get("Fen"), str) or not isinstance(position.get("Moves"), list):
            raise StrengthBoundError(f"{path} contains an invalid opening definition")
        if any(not isinstance(move, str) or not move for move in position["Moves"]):
            raise StrengthBoundError(f"{path} contains an invalid opening move")
        opening_specs[position["Name"]] = (position["Fen"], tuple(position["Moves"]))
    score_by_opening: dict[str, float] = {}
    cap_by_opening: dict[str, bool] = {}
    for index, game in enumerate(games):
        name = game.get("position") if isinstance(game, dict) else None
        position = positions[index]
        if name != names[index] or game.get("initial_fen") != position.get("Fen"):
            raise StrengthBoundError(f"{path} game {index + 1} does not match its opening position")
        if game.get("koi_color") != color:
            raise StrengthBoundError(f"{path} game {index + 1} has wrong candidate color")
        score, capped = _game_result(game, color, path, index)
        try:
            board = chess.Board() if position["Fen"] == "startpos" else chess.Board(position["Fen"])
            recorded_moves = [move["move"] for move in game["moves"]]
            if recorded_moves[:len(position["Moves"])] != position["Moves"]:
                raise StrengthBoundError(f"{path} game {index + 1} does not start with its opening moves")
            if len(recorded_moves) <= len(position["Moves"]):
                raise StrengthBoundError(f"{path} game {index + 1} records no engine moves after its opening")
            for move_text in recorded_moves:
                move = chess.Move.from_uci(move_text)
                if not board.is_legal(move):
                    raise StrengthBoundError(f"{path} game {index + 1} has illegal move {move_text}")
                board.push(move)
        except (ValueError, IndexError) as error:
            raise StrengthBoundError(f"{path} game {index + 1} has illegal move or FEN: {error}") from error
        score_by_opening[name] = score
        cap_by_opening[name] = capped
    pgn = path.with_suffix(".pgn")
    pgn_games = _pgn_games(pgn)
    if len(pgn_games) != len(games):
        raise StrengthBoundError(f"{path} JSON and PGN game counts differ")
    for index, (pgn_result, pgn_moves) in enumerate(pgn_games):
        if pgn_result != games[index]["result"] or pgn_moves != [move["move"] for move in games[index]["moves"]]:
            raise StrengthBoundError(f"{path} JSON and PGN game {index + 1} result or moves differ")
    return score_by_opening, cap_by_opening, normalized, opening_specs


def _elo(score: float) -> float | None:
    if score <= 0.0:
        return None
    if score >= 1.0:
        return None
    return 400.0 * math.log10(score / (1.0 - score))


def _beta_continued_fraction(a: float, b: float, x: float) -> float:
    """Evaluate the continued fraction used by regularized incomplete beta."""
    max_iterations = 300
    epsilon = 3e-14
    tiny = 1e-300
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < tiny:
        d = tiny
    d = 1.0 / d
    result = d
    for iteration in range(1, max_iterations + 1):
        twice = 2 * iteration
        coefficient = iteration * (b - iteration) * x / ((qam + twice) * (a + twice))
        d = 1.0 + coefficient * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + coefficient / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        result *= d * c
        coefficient = -(a + iteration) * (qab + iteration) * x / ((a + twice) * (qap + twice))
        d = 1.0 + coefficient * d
        if abs(d) < tiny:
            d = tiny
        c = 1.0 + coefficient / c
        if abs(c) < tiny:
            c = tiny
        d = 1.0 / d
        delta = d * c
        result *= delta
        if abs(delta - 1.0) < epsilon:
            return result
    raise StrengthBoundError("incomplete-beta calculation did not converge")


def _regularized_beta(x: float, a: float, b: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    front = math.exp(
        math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
        + a * math.log(x) + b * math.log1p(-x)
    )
    if x < (a + 1.0) / (a + b + 2.0):
        return front * _beta_continued_fraction(a, b, x) / a
    return 1.0 - front * _beta_continued_fraction(b, a, 1.0 - x) / b


def _beta_quantile(probability: float, a: float, b: float) -> float:
    low = 0.0
    high = 1.0
    for _ in range(100):
        midpoint = (low + high) / 2.0
        if _regularized_beta(midpoint, a, b) < probability:
            low = midpoint
        else:
            high = midpoint
    return (low + high) / 2.0


def _clopper_pearson_upper(adverse_pairs: int, pair_count: int, alpha: float) -> float:
    if not 0 <= adverse_pairs <= pair_count or pair_count <= 0:
        raise StrengthBoundError("invalid adverse-pair count")
    if adverse_pairs == pair_count:
        return 1.0
    return _beta_quantile(1.0 - alpha, adverse_pairs + 1.0, pair_count - adverse_pairs)


def analyze(white_path: Path, black_path: Path, candidate_hash: str,
            baseline_hash: str, expected: dict[str, Any], alpha: float = 0.05) -> dict[str, Any]:
    candidate_hash = candidate_hash.lower()
    baseline_hash = baseline_hash.lower()
    for label, digest in (("candidate", candidate_hash), ("baseline", baseline_hash)):
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise StrengthBoundError(f"expected {label} SHA-256 must be 64 hexadecimal characters")
    white_scores, white_caps, white_config, white_openings = _load_side(
        white_path, "white", expected, candidate_hash, baseline_hash
    )
    black_scores, black_caps, black_config, black_openings = _load_side(
        black_path, "black", expected, candidate_hash, baseline_hash
    )
    if white_config != black_config:
        raise StrengthBoundError("white and black match configurations differ")
    if white_scores.keys() != black_scores.keys():
        raise StrengthBoundError("white and black matches do not contain the same paired openings")
    if white_openings != black_openings:
        raise StrengthBoundError("white and black matches use different paired opening definitions")
    opening_names = sorted(white_scores)
    if not opening_names:
        raise StrengthBoundError("no paired openings were recorded")
    pair_scores = [(white_scores[name] + black_scores[name]) / 2.0 for name in opening_names]
    mean_score = sum(pair_scores) / len(pair_scores)
    radius = math.sqrt(math.log(1.0 / alpha) / (2.0 * len(pair_scores)))
    lower_score = max(0.0, mean_score - radius)
    lower_elo = _elo(lower_score)
    adverse_count = sum(score < 0.5 for score in pair_scores)
    adverse_upper = _clopper_pearson_upper(adverse_count, len(pair_scores), alpha)
    cp_lower_score = 0.5 * (1.0 - adverse_upper)
    cp_lower_elo = _elo(cp_lower_score)
    threshold = -10.0
    return {
        "schema": "koi-strength-bound-v1",
        "candidate_sha256": candidate_hash,
        "baseline_sha256": baseline_hash,
        "expected_configuration": expected,
        "paired_openings": len(pair_scores),
        "games": 2 * len(pair_scores),
        "wins": sum(score == 1.0 for score in [*white_scores.values(), *black_scores.values()]),
        "draws": sum(score == 0.5 for score in [*white_scores.values(), *black_scores.values()]),
        "losses": sum(score == 0.0 for score in [*white_scores.values(), *black_scores.values()]),
        "capped_games_counted_as_draws": sum(white_caps.values()) + sum(black_caps.values()),
        "mean_score": mean_score,
        "one_sided_confidence": 1.0 - alpha,
        "lower_score_bound": cp_lower_score,
        "lower_elo_bound": cp_lower_elo,
        "threshold_elo": threshold,
        "lower_bound_above_threshold": cp_lower_elo is not None and cp_lower_elo > threshold,
        "hoeffding_lower_score_bound": lower_score,
        "hoeffding_lower_elo_bound": lower_elo,
        "adverse_opening_pairs": adverse_count,
        "adverse_probability_upper_bound": adverse_upper,
        "cp_lower_score_bound": cp_lower_score,
        "cp_lower_elo_bound": cp_lower_elo,
        "decision_bound": "exact_cp_adverse_pair",
        "decision": "pass" if cp_lower_elo is not None and cp_lower_elo > threshold else "inconclusive",
        "method": "Exact one-sided Clopper-Pearson upper bound for adverse paired-opening events (pair score < 0.5), with conservative mean score lower bound 0.5*(1-p_upper). Assumes IID paired-opening draws sampled with replacement: a curated root uniformly at random, then a legal one-ply continuation uniformly at random. The separate Hoeffding score bound is diagnostic only and does not drive the decision. Max-plies games are scored as draws.",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--white", required=True, type=Path, help="candidate-as-White match JSON")
    parser.add_argument("--black", required=True, type=Path, help="candidate-as-Black match JSON")
    parser.add_argument("--candidate-sha256", required=True)
    parser.add_argument("--baseline-sha256", required=True)
    parser.add_argument("--expected-config", required=True, help="JSON object of settings that must match both runs")
    parser.add_argument("--alpha", type=float, default=0.05)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        expected = json.loads(args.expected_config)
        if not isinstance(expected, dict) or not expected:
            raise StrengthBoundError("--expected-config must be a non-empty JSON object")
        if not 0.0 < args.alpha < 1.0:
            raise StrengthBoundError("--alpha must be between zero and one")
        report = analyze(args.white, args.black, args.candidate_sha256,
                         args.baseline_sha256, expected, args.alpha)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (StrengthBoundError, json.JSONDecodeError, OSError) as error:
        print(f"strength bound error: {error}", file=sys.stderr)
        return 2
    print(f"decision={report['decision']} lower_elo_bound={report['lower_elo_bound']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
