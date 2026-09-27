"""Generate local Koi alpha-beta self-play data for policy/value distillation.

The JSONL records use the versioned policy/value v1 contract. Policy targets
are one-hot AlphaBeta principal moves; value and outcome targets come from the
completed self-play game. Datasets and manifests should stay under
``artifacts/training/`` and must not be committed or packaged.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any, Iterable, Iterator

_HERE = Path(__file__).resolve().parent
_REPOSITORY_ROOT = _HERE.parents[1]
_SOURCE_PATHS = (
    "tools/measurement/policy_value_selfplay.py",
    "tools/measurement/policy_value_dataset.py",
    "tools/measurement/koi_chess/__init__.py",
    "tools/measurement/koi_chess/board.py",
    "tools/measurement/koi_chess/core.py",
    "tools/measurement/koi_chess/attacks.py",
    "tools/measurement/koi_chess/engine.py",
    "tools/measurement/koi_chess/pgn.py",
)
_VALUE_SCORE_CP_SCALE = 400.0
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import koi_chess as chess  # noqa: E402
import policy_value_dataset  # noqa: E402


@dataclass(frozen=True)
class Opening:
    """A stable opening identity and its legal UCI move prefix."""

    opening_id: str
    moves: tuple[str, ...]


@dataclass
class GenerationStats:
    selected_openings: list[str] = field(default_factory=list)
    games_completed: int = 0
    records_written: int = 0
    bestmove_fallbacks: int = 0
    outcome_counts: dict[str, int] = field(default_factory=dict)
    termination_counts: dict[str, int] = field(default_factory=dict)


class SelfPlayError(ValueError):
    """Raised when opening data, engine analysis, or dataset output is invalid."""


def read_openings(path: str | Path) -> list[Opening]:
    """Read deterministic ``opening_id | uci moves`` lines."""
    source = Path(path)
    try:
        lines = source.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise SelfPlayError(f"cannot read opening corpus {source}: {error}") from error

    openings: list[Opening] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(lines, 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "|" not in line:
            raise SelfPlayError(f"{source}:{line_number}: expected opening_id | uci moves")
        opening_id, moves_text = (part.strip() for part in line.split("|", 1))
        if not opening_id:
            raise SelfPlayError(f"{source}:{line_number}: opening_id cannot be empty")
        if opening_id in seen:
            raise SelfPlayError(f"{source}:{line_number}: duplicate opening_id {opening_id!r}")
        seen.add(opening_id)
        moves = tuple(moves_text.split())
        for token in moves:
            try:
                chess.Move.from_uci(token)
            except ValueError as error:
                raise SelfPlayError(
                    f"{source}:{line_number}: invalid UCI move {token!r}"
                ) from error
        openings.append(Opening(opening_id, moves))
    if not openings:
        raise SelfPlayError(f"opening corpus contains no entries: {source}")
    return openings


def _apply_opening(board: chess.Board, opening: Opening) -> None:
    for ply, token in enumerate(opening.moves):
        move = chess.Move.from_uci(token)
        if not board.is_legal(move):
            raise SelfPlayError(
                f"opening {opening.opening_id!r} has illegal move {token!r} at ply {ply}"
            )
        board.push(move)


def _action_record(move: chess.Move) -> dict[str, str | None]:
    uci = move.uci()
    return {
        "uci": uci,
        "from": uci[:2],
        "to": uci[2:4],
        "promotion": uci[4] if len(uci) == 5 else None,
    }


def _side_to_move_result(winner: bool | None, side_to_move: bool) -> tuple[str, float]:
    if winner is None:
        return "draw", 0.0
    if winner == side_to_move:
        return "win", 1.0
    return "loss", -1.0


def _score_value_target(analysis: Any, side_to_move: bool) -> float | None:
    """Map an unbounded alpha-beta score to a bounded, STM-relative target."""
    if not isinstance(analysis, dict) or "lowerbound" in analysis or "upperbound" in analysis:
        return None
    score = analysis.get("score")
    if score is None:
        return None
    try:
        relative = score.pov(side_to_move) if hasattr(score, "pov") else score
        if hasattr(relative, "score"):
            centipawns = relative.score(mate_score=2_000)
        else:
            centipawns = float(relative)
        if centipawns is None or not math.isfinite(float(centipawns)):
            return None
    except (TypeError, ValueError, OverflowError):
        return None
    return math.tanh(float(centipawns) / _VALUE_SCORE_CP_SCALE)


def generate_records(
    engine: Any,
    openings: list[Opening],
    *,
    games: int,
    nodes: int,
    seed: int,
    max_plies: int,
    engine_sha256: str,
    opening_corpus_sha256: str,
    source_revision: str,
    engine_version: str,
    cpu_variant: str = "generic",
    nnue_sha256: str | None = None,
    source_dirty: bool = False,
    source_sha256: dict[str, str] | None = None,
    stats: GenerationStats | None = None,
) -> Iterator[dict[str, Any]]:
    """Play bounded Koi-vs-Koi games and emit validated v1 records.

    The AlphaBeta PV's first move supplies a one-hot distillation target. The
    game result supplies side-to-move outcome/value labels. A seeded sample
    selects openings without replacement, so separate train and validation
    runs can use disjoint corpora and retain the trainer's leakage check.
    """
    if games < 1 or nodes < 1 or max_plies < 1:
        raise SelfPlayError("games, nodes, and max_plies must be positive")
    if len(openings) < games:
        raise SelfPlayError(
            f"requested {games} games but the opening corpus has only {len(openings)} entries"
        )
    if not engine_sha256 or not opening_corpus_sha256 or not source_revision or not engine_version:
        raise SelfPlayError("engine, opening, source, and version identities are required")

    run_stats = stats if stats is not None else GenerationStats()
    selected = random.Random(seed).sample(openings, games)
    run_stats.selected_openings[:] = [opening.opening_id for opening in selected]
    search_options = {
        "nodes": nodes,
        "threads": 1,
        "multi_pv": 1,
        "own_book": False,
        "search_algorithm": "AlphaBeta",
        "value_target_transform": "tanh(cp/400) when an unbounded score is present; otherwise game result",
        "engine_sha256": engine_sha256,
        "opening_corpus_sha256": opening_corpus_sha256,
        "source_revision": source_revision,
        "source_dirty": source_dirty,
        "source_sha256": source_sha256 or {},
        "cpu_variant": cpu_variant,
        "nnue_sha256": nnue_sha256,
        "seed": seed,
        "max_plies": max_plies,
    }

    for game_number, opening in enumerate(selected, 1):
        board = chess.Board()
        _apply_opening(board, opening)
        if len(board.move_stack) >= max_plies:
            raise SelfPlayError(
                f"max_plies {max_plies} must leave room after opening {opening.opening_id!r}"
            )
        if board.outcome(claim_draw=True) is not None:
            raise SelfPlayError(f"opening {opening.opening_id!r} already ends the game")

        pending: list[dict[str, Any]] = []
        game_outcome = None
        while len(board.move_stack) < max_plies:
            game_outcome = board.outcome(claim_draw=True)
            if game_outcome is not None:
                break

            python_legal_moves = list(board.legal_moves)
            if not python_legal_moves:
                game_outcome = board.outcome()
                break
            try:
                legal_moves = engine.legal_moves(board)
            except chess.engine.EngineError as error:
                raise SelfPlayError(
                    f"native legal move query failed in game {game_number}, ply {len(board.move_stack)}: {error}"
                ) from error
            native_uci = [move.uci() for move in legal_moves]
            python_uci = {move.uci() for move in python_legal_moves}
            if len(native_uci) != len(set(native_uci)) or set(native_uci) != python_uci:
                raise SelfPlayError(
                    f"native and dataset-side legal move sets differ in game {game_number}, ply {len(board.move_stack)}"
                )
            actions = [_action_record(move) for move in legal_moves]

            try:
                analysis = engine.analyse(board, chess.engine.Limit(nodes=nodes))
            except chess.engine.EngineError as error:
                raise SelfPlayError(
                    f"AlphaBeta search failed in game {game_number}, ply {len(board.move_stack)}: {error}"
                ) from error
            principal_variation = analysis.get("pv") if isinstance(analysis, dict) else None
            if not principal_variation:
                try:
                    play_result = engine.play(board, chess.engine.Limit(nodes=nodes))
                except chess.engine.EngineError as error:
                    raise SelfPlayError(
                        f"AlphaBeta bestmove fallback failed in game {game_number}, ply {len(board.move_stack)}: {error}"
                    ) from error
                best_move = getattr(play_result, "move", None)
                policy_target_source = "bestmove-fallback"
                if best_move is None:
                    raise SelfPlayError(
                        f"AlphaBeta returned neither a PV nor bestmove in game {game_number}, ply {len(board.move_stack)}"
                    )
                run_stats.bestmove_fallbacks += 1
            else:
                best_move = principal_variation[0]
                policy_target_source = "pv"
            score_value_target = _score_value_target(analysis, board.turn)
            value_target_source = (
                "alphabeta-score" if score_value_target is not None else "game-result-fallback"
            )
            if best_move not in python_legal_moves:
                raise SelfPlayError(
                    f"AlphaBeta returned illegal move {best_move} in game {game_number}, ply {len(board.move_stack)}"
                )
            best_uci = best_move.uci()
            try:
                best_action_index = next(
                    index for index, action in enumerate(actions)
                    if action["uci"] == best_uci
                )
            except StopIteration as error:
                raise SelfPlayError(
                    f"AlphaBeta move {best_uci!r} is absent from the complete legal action list"
                ) from error
            policy_targets = [0.0] * len(actions)
            policy_targets[best_action_index] = 1.0
            pending.append({
                "position": {"fen": board.fen(), "variant": "standard"},
                "legal_actions": actions,
                "policy_targets": policy_targets,
                "opening_id": opening.opening_id,
                "seed": seed,
                "ply": len(board.move_stack),
                "side_to_move": board.turn,
                "policy_target_source": policy_target_source,
                "score_value_target": score_value_target,
                "value_target_source": value_target_source,
            })
            board.push(best_move)

        if not pending:
            raise SelfPlayError(f"opening {opening.opening_id!r} produced no training positions")
        if game_outcome is None:
            game_outcome = board.outcome(claim_draw=True)
        if game_outcome is None:
            termination_reason = "ply-limit"
            winner = None
            game_result = "draw"
        else:
            termination_reason = game_outcome.termination.name.lower()
            winner = game_outcome.winner
            game_result = (
                "draw" if winner is None else
                "white-win" if winner == chess.WHITE else "black-win"
            )

        game_slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", opening.opening_id).strip("-")[:48]
        game_id = f"ab-{seed}-{game_slug or 'opening'}-{game_number:06d}"
        for position in pending:
            outcome, value = _side_to_move_result(winner, position.pop("side_to_move"))
            policy_target_source = position.pop("policy_target_source")
            score_value_target = position.pop("score_value_target")
            value_target_source = position.pop("value_target_source")
            record = {
                "schema": policy_value_dataset.SCHEMA,
                "action_encoding": policy_value_dataset.ACTION_ENCODING,
                **position,
                "value_target": value if score_value_target is None else score_value_target,
                "outcome": outcome,
                "game_id": game_id,
                "feature_schema": "halfka-threat-v5",
                "producer": {
                    "kind": "alpha-beta-distillation",
                    "name": "Koi Engine",
                    "version": engine_version,
                },
                "search_provenance": {
                    "algorithm": "AlphaBeta",
                    "options": {
                        **search_options,
                        "policy_target_source": policy_target_source,
                        "value_target_source": value_target_source,
                    },
                },
                "network_sha256": None,
                "termination_reason": termination_reason,
            }
            policy_value_dataset.validate_record(record)
            run_stats.records_written += 1
            yield record

        run_stats.games_completed += 1
        run_stats.outcome_counts[game_result] = run_stats.outcome_counts.get(game_result, 0) + 1
        run_stats.termination_counts[termination_reason] = (
            run_stats.termination_counts.get(termination_reason, 0) + 1
        )


def _sha256_file(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_json_atomic(path: str | Path, value: dict[str, Any]) -> None:
    destination = Path(path)
    temporary_path: Path | None = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        encoded = json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + "\n"
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=destination.parent,
            prefix=f".{destination.name}.", suffix=".tmp", delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, destination)
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def write_dataset(
    output_path: str | Path,
    manifest_path: str | Path,
    records: Iterable[dict[str, Any]],
    manifest_fields: dict[str, Any],
    *,
    overwrite: bool = False,
) -> dict[str, Any]:
    """Atomically write JSONL plus a provenance manifest with content hash."""
    output = Path(output_path)
    manifest = Path(manifest_path)
    if output.resolve() == manifest.resolve():
        raise SelfPlayError("dataset and manifest must use distinct paths")
    if not overwrite and (output.exists() or manifest.exists()):
        raise SelfPlayError("dataset or manifest already exists; choose new paths or pass overwrite=True")
    if any(key in manifest_fields for key in ("schema", "dataset_sha256", "record_count")):
        raise SelfPlayError("manifest_fields cannot override generated schema or dataset identity")

    record_count = 0
    iterator = iter(records)
    try:
        first_record = next(iterator)
    except StopIteration as error:
        raise SelfPlayError("cannot write an empty policy/value dataset") from error

    def counted_records() -> Iterator[dict[str, Any]]:
        nonlocal record_count
        record_count += 1
        yield first_record
        for record in iterator:
            record_count += 1
            yield record

    policy_value_dataset.write_jsonl(output, counted_records())
    result = {
        "schema": "koi-policy-value-selfplay-manifest-v1",
        **manifest_fields,
        "dataset_sha256": _sha256_file(output),
        "record_count": record_count,
    }
    _write_json_atomic(manifest, result)
    return result


def _source_revision(repository_root: Path) -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=repository_root,
            check=True, capture_output=True, text=True, timeout=5.0,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def _source_provenance(repository_root: Path) -> tuple[bool, dict[str, str]]:
    source_hashes = {
        relative: _sha256_file(repository_root / relative)
        for relative in _SOURCE_PATHS
    }
    try:
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all", "--", *_SOURCE_PATHS],
            cwd=repository_root, check=True, capture_output=True, text=True, timeout=5.0,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return True, source_hashes
    return bool(status.strip()), source_hashes


def _engine_path(value: str | Path) -> Path:
    path = Path(value)
    if path.is_file():
        return path.resolve()
    resolved = shutil.which(str(value))
    if resolved:
        return Path(resolved).resolve()
    raise SelfPlayError(f"engine executable does not exist: {value}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, help="path to the Koi UCI engine executable")
    parser.add_argument(
        "--openings", type=Path,
        default=_REPOSITORY_ROOT / "tests/data/openings/openings-curated-32.txt",
        help="opening_id | UCI-move-prefix corpus (default: tests/data/openings/openings-curated-32.txt)",
    )
    parser.add_argument("--games", type=int, default=1, help="number of distinct openings to self-play")
    parser.add_argument("--nodes", type=int, default=20_000, help="AlphaBeta nodes per move")
    parser.add_argument("--max-plies", type=int, default=256, help="maximum total plies per game")
    parser.add_argument("--seed", type=int, default=1, help="opening-sample seed stored in every record")
    parser.add_argument("--hash-mb", type=int, default=512)
    parser.add_argument(
        "--cpu-variant", choices=("generic", "avx2", "avx512"), default="generic",
        help="force the engine CPU variant for reproducible runs",
    )
    parser.add_argument("--engine-version", default="source-" + _source_revision(_REPOSITORY_ROOT)[:12])
    parser.add_argument(
        "--output", type=Path,
        default=_REPOSITORY_ROOT / "artifacts/training/policy-value-ab-selfplay-v1.jsonl",
        help="local JSONL output; keep generated data under artifacts/training/",
    )
    parser.add_argument("--manifest", type=Path, help="manifest path (defaults beside the JSONL output)")
    parser.add_argument("--overwrite", action="store_true", help="replace existing local output files")
    args = parser.parse_args(argv)

    try:
        if args.games < 1 or args.nodes < 1 or args.max_plies < 1 or args.hash_mb < 1:
            raise SelfPlayError("games, nodes, max_plies, and hash_mb must be positive")
        openings = read_openings(args.openings)
        if args.games > len(openings):
            raise SelfPlayError(
                f"requested {args.games} games but the opening corpus has only {len(openings)} entries"
            )
        engine_path = _engine_path(args.engine)
        engine_sha256 = _sha256_file(engine_path)
        opening_corpus_sha256 = _sha256_file(args.openings)
        source_revision = _source_revision(_REPOSITORY_ROOT)
        source_dirty, source_sha256 = _source_provenance(_REPOSITORY_ROOT)
        environment = os.environ.copy()
        environment["KOI_CPU_VARIANT"] = args.cpu_variant
        nnue_environment_path = environment.get("KOI_NNUE_PATH", "")
        nnue_path = Path(nnue_environment_path) if nnue_environment_path else engine_path.parent / "koi.nnue"
        nnue_sha256 = _sha256_file(nnue_path) if nnue_path.is_file() else None
        manifest_path = args.manifest or args.output.with_suffix(args.output.suffix + ".manifest.json")

        if not args.overwrite and (args.output.exists() or manifest_path.exists()):
            raise SelfPlayError("dataset or manifest already exists; choose new paths or pass --overwrite")

        stats = GenerationStats()
        with chess.engine.SimpleEngine.popen_uci(engine_path, env=environment) as engine:
            engine.configure({
                "OwnBook": False,
                "Threads": 1,
                "Hash": args.hash_mb,
                "Speed": 100,
                "MultiPV": 1,
                "SearchAlgorithm": "AlphaBeta",
                "PolicyValueFile": "",
            })
            manifest_fields: dict[str, Any] = {
                "engine_sha256": engine_sha256,
                "opening_corpus_sha256": opening_corpus_sha256,
                "source_revision": source_revision,
                "source_dirty": source_dirty,
                "source_sha256": source_sha256,
                "engine_version": args.engine_version,
                "cpu_variant": args.cpu_variant,
                "boot_nnue_path": str(nnue_path) if nnue_sha256 else None,
                "nnue_sha256": nnue_sha256,
                "seed": args.seed,
                "games_requested": args.games,
                "search_options": {
                    "nodes": args.nodes,
                    "threads": 1,
                    "hash_mb": args.hash_mb,
                    "multi_pv": 1,
                    "own_book": False,
                    "search_algorithm": "AlphaBeta",
                        "value_target_transform": "tanh(cp/400) when an unbounded score is present; otherwise game result",
                    "max_plies": args.max_plies,
                },
            }

            def records_with_manifest_stats() -> Iterator[dict[str, Any]]:
                yield from generate_records(
                    engine,
                    openings,
                    games=args.games,
                    nodes=args.nodes,
                    seed=args.seed,
                    max_plies=args.max_plies,
                    engine_sha256=engine_sha256,
                    opening_corpus_sha256=opening_corpus_sha256,
                    source_revision=source_revision,
                    engine_version=args.engine_version,
                    cpu_variant=args.cpu_variant,
                    nnue_sha256=nnue_sha256,
                    source_dirty=source_dirty,
                    source_sha256=source_sha256,
                    stats=stats,
                )
                manifest_fields.update({
                    "games_completed": stats.games_completed,
                    "records_written": stats.records_written,
                    "bestmove_fallbacks": stats.bestmove_fallbacks,
                    "outcome_counts": stats.outcome_counts,
                    "termination_counts": stats.termination_counts,
                    "selected_openings": stats.selected_openings,
                })

            manifest = write_dataset(
                args.output,
                manifest_path,
                records_with_manifest_stats(),
                manifest_fields,
                overwrite=args.overwrite,
            )
        print(json.dumps(manifest, sort_keys=True))
        return 0
    except (SelfPlayError, policy_value_dataset.DatasetError, chess.engine.EngineError,
            OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
