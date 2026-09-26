"""Validate Cute Chess PGN and stability-report artifacts for release gates.

This script intentionally uses only the Python standard library and the
repository's koi_chess compatibility library, so both Windows and Linux CI can
run the same artifact checks.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from collections import Counter


REPOSITORY_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPOSITORY_ROOT / "tools" / "measurement"))
import koi_chess as chess  # noqa: E402


OPENING_LINE = re.compile(
    r"^(?P<name>[^|#\s]+)\s*\|\s*(?P<moves>[a-h][1-8][a-h][1-8][nbrq]?(?:\s+[a-h][1-8][a-h][1-8][nbrq]?)*)\s*$"
)
RESULTS = {"1-0", "0-1", "1/2-1/2"}
UCI_MOVE = re.compile(r"^[a-h][1-8][a-h][1-8][nbrq]?$")


def _read_openings(path: pathlib.Path, errors: list[str]) -> dict[str, tuple[str, ...]]:
    openings: dict[str, tuple[str, ...]] = {}
    try:
        lines = path.read_text(encoding="utf-8-sig").splitlines()
    except OSError as error:
        errors.append(f"opening file could not be read: {error}")
        return openings

    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = OPENING_LINE.fullmatch(line)
        if not match:
            errors.append(f"opening file line {number} is malformed")
            continue
        name = match.group("name")
        if name in openings:
            errors.append(f"opening file repeats entry '{name}'")
            continue
        moves = tuple(match.group("moves").split())
        board = chess.Board()
        legal = True
        for move_text in moves:
            try:
                move = chess.Move.from_uci(move_text)
                if not board.is_legal(move):
                    legal = False
                    break
                board.push(move)
            except (ValueError, IndexError):
                legal = False
                break
        if not legal:
            errors.append(f"opening '{name}' contains an illegal UCI move")
            continue
        openings[name] = moves
    if not openings:
        errors.append("opening file contains no valid entries")
    return openings


def _load_json(path: pathlib.Path, description: str, errors: list[str]):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"{description} could not be read as JSON: {error}")
        return None


def _pgn_games(path: pathlib.Path, errors: list[str]):
    games = []
    try:
        with path.open("r", encoding="utf-8-sig") as stream:
            while game := chess.pgn.read_game(stream):
                games.append(game)
    except (OSError, UnicodeError) as error:
        errors.append(f"PGN could not be read: {error}")
    return games


def _uci_pgn_games(path: pathlib.Path, errors: list[str]):
    try:
        text = path.read_text(encoding="utf-8-sig")
    except (OSError, UnicodeError) as error:
        errors.append(f"PGN could not be read: {error}")
        return []
    blocks = [block for block in re.split(r"(?m)(?=^\[Event )", text.strip()) if block.strip()]
    parsed = []
    for index, block in enumerate(blocks, 1):
        if '[MoveFormat "UCI coordinate notation"]' not in block:
            errors.append(f"PGN game {index} is missing its UCI coordinate move format")
        result_match = re.search(r'(?m)^\[Result\s+"([^"]+)"\]\s*$', block)
        result = result_match.group(1) if result_match else None
        if "\n\n" not in block:
            errors.append(f"PGN game {index} has no movetext")
            movetext = ""
        else:
            movetext = block.split("\n\n", 1)[1]
        movetext = re.sub(r"\{[^}]*\}|;[^\n]*", " ", movetext)
        tokens = re.findall(r"(?<![A-Za-z0-9])[a-h][1-8][a-h][1-8][nbrq]?(?![A-Za-z0-9])", movetext)
        parsed.append({"moves": tokens, "result": result})
    return parsed


def _validate_uci_match(
    pgn_file: pathlib.Path,
    report: dict,
    openings_file: pathlib.Path,
    *,
    expected_games: int,
    expected_color: str,
    expected_time_control: str,
    expected_threads: int,
    expected_max_moves: int,
    expected_opening_count: int,
) -> list[str]:
    errors: list[str] = []
    openings = _read_openings(openings_file, errors) if expected_opening_count else {}
    if len(openings) != expected_opening_count:
        errors.append(
            f"opening file has {len(openings)} valid entries; expected {expected_opening_count}"
        )
    configuration = report.get("configuration")
    games = report.get("games")
    if not isinstance(configuration, dict):
        errors.append("UCI match report has no configuration object")
        configuration = {}
    if not isinstance(games, list):
        errors.append("UCI match report has no games array")
        games = []
    if len(games) != expected_games:
        errors.append(f"report contains {len(games)} games; expected {expected_games}")
    expected_configuration = {
        "max_plies": expected_max_moves,
        "koi_color": expected_color,
        "time_control": expected_time_control,
        "threads": expected_threads,
        "koi_own_book": False,
    }
    for key, value in expected_configuration.items():
        if configuration.get(key) != value:
            errors.append(f"configuration {key} is {configuration.get(key)!r}; expected {value!r}")

    pgn_games = _uci_pgn_games(pgn_file, errors)
    if len(pgn_games) != expected_games:
        errors.append(f"PGN contains {len(pgn_games)} games; expected {expected_games}")
    if len(pgn_games) != len(games):
        errors.append(f"PGN/report game counts differ ({len(pgn_games)} versus {len(games)})")

    opening_counts: Counter[str] = Counter()
    allowed_terminations = {
        "max plies", "checkmate", "stalemate", "threefold repetition",
        "fivefold repetition", "fifty-move rule", "75-move rule",
        "insufficient material", "resignation", "rule draw",
    }
    for index, game in enumerate(games, 1):
        if not isinstance(game, dict):
            errors.append(f"report game {index} is not an object")
            continue
        game_moves = game.get("moves")
        if not isinstance(game_moves, list):
            errors.append(f"report game {index} has no move array")
            game_moves = []
        if index <= len(pgn_games):
            pgn_game = pgn_games[index - 1]
            json_moves = [record.get("move") for record in game_moves if isinstance(record, dict)]
            if pgn_game["moves"] != json_moves:
                errors.append(f"PGN game {index} moves do not match the JSON move records")
            if pgn_game["result"] != game.get("result"):
                errors.append(f"PGN game {index} result does not match the JSON move record")

        initial_fen = game.get("initial_fen", game.get("position", "startpos"))
        try:
            board = chess.Board() if initial_fen == "startpos" else chess.Board(str(initial_fen))
        except ValueError as error:
            errors.append(f"game {index} has invalid initial FEN: {error}")
            continue
        move_texts = [
            record.get("move") for record in game_moves if isinstance(record, dict)
        ]
        opening_name = None
        opening_plies = 0
        if openings:
            recorded_name = game.get("position")
            if recorded_name not in openings:
                errors.append(f"report game {index} names an unknown curated opening {recorded_name!r}")
            else:
                opening_moves = openings[recorded_name]
                expected_board = chess.Board()
                for move_text in opening_moves:
                    expected_board.push(chess.Move.from_uci(move_text))
                if initial_fen != "startpos" and board.fen() == expected_board.fen():
                    opening_name = recorded_name
                elif tuple(move_texts[: len(opening_moves)]) == opening_moves:
                    opening_name = recorded_name
                    opening_plies = len(opening_moves)
            if opening_name is None:
                errors.append(f"report game {index} does not match a curated opening")
            else:
                opening_counts[opening_name] += 1
        plies_after_opening = len(game_moves) - opening_plies
        if plies_after_opening < 1:
            errors.append(f"game {index} has no engine moves after its opening")

        termination = game.get("termination")
        result = game.get("result")
        if game.get("koi_color") != expected_color:
            errors.append(
                f"game {index} Koi color is {game.get('koi_color')!r}; expected {expected_color!r}"
            )
        if termination not in allowed_terminations:
            errors.append(f"game {index} has failing or unknown termination {termination!r}")
        for key in (
            "failure", "failures", "errors", "protocol_errors", "crashes",
            "illegal_moves", "missing_bestmoves", "duplicate_bestmoves",
        ):
            if game.get(key):
                errors.append(f"game {index} reports {key.replace('_', ' ')}")
        if result == "*":
            if termination != "max plies" or plies_after_opening != expected_max_moves:
                errors.append(
                    f"game {index} is unfinished without reaching the configured max-plies termination"
                )
        elif result not in RESULTS:
            errors.append(f"game {index} has invalid result {result!r}")
        if termination == "max plies" and plies_after_opening != expected_max_moves:
            errors.append(
                f"game {index} ended at {plies_after_opening} plies after its opening; "
                f"expected exactly {expected_max_moves}"
            )
        if plies_after_opening > expected_max_moves:
            errors.append(f"game {index} exceeds the {expected_max_moves}-ply cap")

        process_status = game.get("process_status")
        if not isinstance(process_status, dict) or not process_status:
            errors.append(f"game {index} has no engine process status")
        elif any(label not in process_status for label in ("koi", "opponent")):
            missing = ", ".join(label for label in ("koi", "opponent") if label not in process_status)
            errors.append(f"game {index} is missing {missing} process status")
        elif any(status != "clean shutdown" for status in process_status.values()):
            errors.append(f"game {index} has an engine crash or unclean process status")

        for ply, move_record in enumerate(game_moves, 1):
            if not isinstance(move_record, dict):
                errors.append(f"game {index} ply {ply} is not a move record")
                continue
            move_text = move_record.get("move")
            if not isinstance(move_text, str) or not UCI_MOVE.fullmatch(move_text):
                errors.append(f"game {index} ply {ply} has invalid UCI move {move_text!r}")
                continue
            try:
                move = chess.Move.from_uci(move_text)
                if move_record.get("replay_legal") is not True or not board.is_legal(move):
                    errors.append(f"game {index} ply {ply} contains an illegal move")
                    continue
                if move_record.get("side") != ("w" if board.turn else "b"):
                    errors.append(f"game {index} ply {ply} has an incorrect side-to-move record")
                board.push(move)
            except (ValueError, IndexError) as error:
                errors.append(f"game {index} ply {ply} cannot be replayed: {error}")
                continue
            bestmove = move_record.get("bestmove_line")
            if opening_name is not None and ply <= opening_plies:
                if move_record.get("engine_label") != "opening" or bestmove:
                    errors.append(f"game {index} ply {ply} has an invalid curated opening record")
            else:
                expected_bestmove = re.fullmatch(
                    rf"bestmove\s+{re.escape(move_text)}(?:\s+ponder\s+[a-h][1-8][a-h][1-8][nbrq]?)?",
                    str(bestmove or ""),
                )
                if expected_bestmove is None:
                    errors.append(f"game {index} ply {ply} has missing, duplicate, or mismatched bestmove")
            if move_record.get("book_used") is True:
                errors.append(f"game {index} ply {ply} used the Koi opening book")

    missing_openings = sorted(set(openings) - set(opening_counts))
    if missing_openings:
        errors.append("report does not cover curated openings: " + ", ".join(missing_openings))
    return errors


def _opening_match(game, openings: dict[str, tuple[str, ...]]) -> tuple[str, int] | None:
    root = game.board()
    root_fen = root.fen()
    for name, uci_moves in openings.items():
        expected = chess.Board()
        for move_text in uci_moves:
            expected.push(chess.Move.from_uci(move_text))
        if root_fen == expected.fen():
            return name, 0

    # Cute Chess/EPD versions differ in whether they keep the selected opening
    # in the PGN SetUp/FEN headers or replay its moves in the mainline.
    played_moves = [node.move.uci() for node in game.mainline()]
    for name, uci_moves in openings.items():
        if tuple(played_moves[: len(uci_moves)]) == uci_moves:
            return name, len(uci_moves)
    return None


def validate_match_artifacts(
    pgn_path: pathlib.Path | str,
    report_path: pathlib.Path | str,
    openings_path: pathlib.Path | str,
    *,
    expected_games: int,
    expected_color: str,
    expected_time_control: str,
    expected_threads: int,
    expected_max_moves: int,
    expected_opening_count: int = 32,
) -> list[str]:
    """Return every artifact inconsistency; an empty list means the leg passed."""
    pgn_file = pathlib.Path(pgn_path)
    report_file = pathlib.Path(report_path)
    openings_file = pathlib.Path(openings_path)
    errors: list[str] = []
    report = _load_json(report_file, "stability report", errors)
    if isinstance(report, dict) and report.get("schema") == "koi-uci-match-v2":
        return _validate_uci_match(
            pgn_file,
            report,
            openings_file,
            expected_games=expected_games,
            expected_color=expected_color,
            expected_time_control=expected_time_control,
            expected_threads=expected_threads,
            expected_max_moves=expected_max_moves,
            expected_opening_count=expected_opening_count,
        )
    if report is not None and not isinstance(report, dict):
        errors.append("stability report root must be a JSON object")
        report = None
    openings = _read_openings(openings_file, errors) if expected_opening_count else {}
    games = _pgn_games(pgn_file, errors)

    if expected_color not in {"white", "black"}:
        errors.append("expected color must be white or black")
    if expected_games < 1:
        errors.append("expected game count must be positive")
    if len(openings) != expected_opening_count:
        errors.append(
            f"opening file has {len(openings)} valid entries; expected {expected_opening_count}"
        )
    if len(games) != expected_games:
        errors.append(f"PGN contains {len(games)} games; expected {expected_games}")

    if report is not None:
        if report.get("schema") != "koi-cutechess-stability-v1":
            errors.append("stability report has an unexpected schema")
        configuration = report.get("configuration")
        results = report.get("results")
        if not isinstance(configuration, dict):
            errors.append("stability report has no configuration object")
            configuration = {}
        if not isinstance(results, dict):
            errors.append("stability report has no results object")
            results = {}

        expected_configuration = {
            "games": expected_games,
            "koi_color": expected_color,
            "time_control": expected_time_control,
            "threads": expected_threads,
            "max_moves": expected_max_moves,
            "own_book": False,
        }
        for key, value in expected_configuration.items():
            if configuration.get(key) != value:
                errors.append(
                    f"configuration {key} is {configuration.get(key)!r}; expected {value!r}"
                )

        if results.get("exit_code") != 0:
            errors.append(f"Cute Chess process exit code is {results.get('exit_code')!r}; expected 0")
        if results.get("termination_classification") != "completed":
            errors.append(
                "Cute Chess termination is "
                f"{results.get('termination_classification')!r}; expected 'completed'"
            )
        for key in ("started_games", "finished_games"):
            if results.get(key) != expected_games:
                errors.append(f"report {key} is {results.get(key)!r}; expected {expected_games}")
        if results.get("failures") != 0:
            errors.append(f"report records {results.get('failures')!r} failures; expected 0")
        if results.get("failure_lines"):
            errors.append("report contains failure lines")
        expected_white = expected_games if expected_color == "white" else 0
        expected_black = expected_games if expected_color == "black" else 0
        if (results.get("koi_white") != expected_white or
                results.get("koi_black") != expected_black):
            errors.append(
                "actual Koi color counts are "
                f"white={results.get('koi_white')!r}, black={results.get('koi_black')!r}; "
                f"expected white={expected_white}, black={expected_black}"
            )

    opening_counts: Counter[str] = Counter()
    for index, game in enumerate(games, 1):
        actual_koi_color = (
            "white" if game.headers.get("White") == "Koi" else
            "black" if game.headers.get("Black") == "Koi" else "missing"
        )
        if actual_koi_color != expected_color:
            errors.append(
                f"PGN game {index} has Koi as {actual_koi_color}; expected {expected_color}"
            )
        if game.errors:
            errors.append(f"PGN game {index} contains illegal or unparseable moves: {game.errors[0]}")
        result = game.headers.get("Result", "*")
        if result not in RESULTS:
            errors.append(f"PGN game {index} is unfinished or has invalid result {result!r}")
        opening_plies_in_movetext = 0
        if openings:
            opening_match = _opening_match(game, openings)
            if opening_match is None:
                errors.append(f"PGN game {index} does not match a curated opening")
            else:
                opening_name, opening_plies_in_movetext = opening_match
                opening_counts[opening_name] += 1
        ply_count = sum(1 for _ in game.mainline()) - opening_plies_in_movetext
        if ply_count < 1:
            errors.append(f"PGN game {index} has no engine moves after its opening")
        # Cute Chess 1.5.1's GameAdjudicator::addEval returns before checking
        # -maxmoves when an engine reports no search depth. Real completed
        # matches can thus contain one extra ply from either starting side.
        # Permit one delayed check, but still reject a longer overrun.
        max_engine_plies = 2 * expected_max_moves + 1
        if ply_count > max_engine_plies:
            errors.append(
                f"PGN game {index} has {ply_count} plies after its opening; "
                f"expected at most {max_engine_plies}"
            )

    missing_openings = sorted(set(openings) - set(opening_counts))
    if missing_openings:
        errors.append("PGN does not cover curated openings: " + ", ".join(missing_openings))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pgn", required=True, type=pathlib.Path)
    parser.add_argument("--report", required=True, type=pathlib.Path)
    parser.add_argument("--openings", required=True, type=pathlib.Path)
    parser.add_argument("--expected-games", required=True, type=int)
    parser.add_argument("--expected-color", required=True, choices=("white", "black"))
    parser.add_argument("--expected-time-control", required=True)
    parser.add_argument("--expected-threads", required=True, type=int)
    parser.add_argument("--expected-max-moves", required=True, type=int)
    parser.add_argument(
        "--expected-opening-count",
        type=int,
        default=32,
        help="require this many distinct opening roots; pass 0 to disable opening coverage",
    )
    args = parser.parse_args(argv)

    errors = validate_match_artifacts(
        args.pgn,
        args.report,
        args.openings,
        expected_games=args.expected_games,
        expected_color=args.expected_color,
        expected_time_control=args.expected_time_control,
        expected_threads=args.expected_threads,
        expected_max_moves=args.expected_max_moves,
        expected_opening_count=args.expected_opening_count,
    )
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    schema = json.loads(args.report.read_text(encoding="utf-8-sig")).get("schema")
    limit_description = (
        f"max plies={args.expected_max_moves}" if schema == "koi-uci-match-v2"
        else (
            f"max full moves={args.expected_max_moves} "
            f"(up to {2 * args.expected_max_moves + 1} "
            "plies with one cap-check delay)"
        )
    )
    summary = (
        f"PASS: {args.expected_games} legal, completed games; "
        f"Koi {args.expected_color}; {args.expected_time_control}; "
        f"Threads={args.expected_threads}; OwnBook=false; "
        f"{limit_description}"
    )
    if args.expected_opening_count:
        summary += f"; all {args.expected_opening_count} curated openings covered"
    print(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
