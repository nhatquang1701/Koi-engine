"""Generate deterministic, disjoint Koi policy/value opening corpora.

Generated roots are legal positions selected with a seeded random walk. Roots
that match any state in the supplied held-out match corpora are rejected.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import sys
import tempfile
from typing import Iterable

_HERE = Path(__file__).resolve().parent
_REPOSITORY_ROOT = _HERE.parents[1]
_DATA_DIR = _HERE / "data"
_DEFAULT_TRAIN_OUTPUT = _DATA_DIR / "policy-value-train-v2.txt"
_DEFAULT_VALIDATION_OUTPUT = _DATA_DIR / "policy-value-validation-v2.txt"
_DEFAULT_MANIFEST = _DATA_DIR / "policy-value-openings-v2.manifest.json"
_DEFAULT_MATCH_CORPORA = (
    _REPOSITORY_ROOT / "tests/data/openings/openings-curated-32.txt",
    _REPOSITORY_ROOT / "tests/data/openings/openings-release-strength-160.txt",
)

if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import koi_chess as chess  # noqa: E402
from koi_chess import Move  # noqa: E402
import policy_value_selfplay  # noqa: E402

Opening = policy_value_selfplay.Opening


class OpeningCorpusError(ValueError):
    """An opening corpus could not be generated or validated."""


def position_state(board: chess.Board) -> tuple[str, ...]:
    """Return the trainer's rules-relevant standard-chess root identity."""
    return ("standard", *board.fen().split()[:5])


def heldout_position_states(openings: Iterable[Opening]) -> set[tuple[str, ...]]:
    """Collect the initial and every played state from held-out openings."""
    states: set[tuple[str, ...]] = set()
    for opening in openings:
        board = chess.Board()
        states.add(position_state(board))
        for token in opening.moves:
            move = Move.from_uci(token)
            if not board.is_legal(move):
                raise OpeningCorpusError(
                    f"held-out opening {opening.opening_id!r} has illegal move {token!r}"
                )
            board.push(move)
            states.add(position_state(board))
    return states


def _draw_candidate(
    rng: random.Random, min_plies: int, max_plies: int
) -> tuple[tuple[str, ...], tuple[str, ...]] | None:
    target_plies = rng.randint(min_plies, max_plies)
    board = chess.Board()
    moves: list[str] = []
    for _ in range(target_plies):
        if board.outcome(claim_draw=True) is not None:
            return None
        legal_moves = sorted(board.legal_moves, key=lambda move: move.uci())
        if not legal_moves:
            return None
        move = legal_moves[rng.randrange(len(legal_moves))]
        moves.append(move.uci())
        board.push(move)
    if board.outcome(claim_draw=True) is not None:
        return None
    return tuple(moves), position_state(board)


def _generate_split(
    split: str,
    count: int,
    rng: random.Random,
    used_states: set[tuple[str, ...]],
    *,
    min_plies: int,
    max_plies: int,
    max_attempts: int,
) -> list[Opening]:
    generated: list[Opening] = []
    for _ in range(max_attempts):
        if len(generated) == count:
            break
        candidate = _draw_candidate(rng, min_plies, max_plies)
        if candidate is None:
            continue
        moves, root_state = candidate
        if root_state in used_states:
            continue
        opening_id = f"policy-v2-{split}-{len(generated) + 1:04d}"
        generated.append(Opening(opening_id, moves))
        used_states.add(root_state)
    if len(generated) != count:
        raise OpeningCorpusError(
            f"could not generate {count} disjoint {split} openings after "
            f"{max_attempts} legal-prefix attempts (generated {len(generated)})"
        )
    return generated


def generate_opening_splits(
    match_openings: Iterable[Opening],
    *,
    train_count: int,
    validation_count: int,
    seed: int,
    min_plies: int = 6,
    max_plies: int = 18,
    max_attempts_per_split: int = 4096,
) -> tuple[list[Opening], list[Opening]]:
    """Create reproducible train/validation roots disjoint from match states."""
    openings = list(match_openings)
    if not openings:
        raise OpeningCorpusError("at least one held-out match opening is required")
    if train_count < 1 or validation_count < 1:
        raise OpeningCorpusError("train_count and validation_count must be positive")
    if seed < 0:
        raise OpeningCorpusError("seed must be non-negative")
    if min_plies < 1 or max_plies < min_plies:
        raise OpeningCorpusError("opening ply bounds must satisfy 1 <= min_plies <= max_plies")
    if max_attempts_per_split < 1:
        raise OpeningCorpusError("max_attempts_per_split must be positive")

    used_states = heldout_position_states(openings)
    rng = random.Random(seed)
    train = _generate_split(
        "train", train_count, rng, used_states,
        min_plies=min_plies, max_plies=max_plies,
        max_attempts=max_attempts_per_split,
    )
    validation = _generate_split(
        "validation", validation_count, rng, used_states,
        min_plies=min_plies, max_plies=max_plies,
        max_attempts=max_attempts_per_split,
    )
    return train, validation


def _sha256_bytes(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def _manifest_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(_REPOSITORY_ROOT).as_posix()
    except ValueError:
        return str(resolved)


def _serialize_openings(
    split: str,
    openings: list[Opening],
    *,
    seed: int,
    min_plies: int,
    max_plies: int,
    heldout_states_sha256: str,
) -> bytes:
    lines = [
        "# Koi policy/value opening corpus v2",
        f"# split={split}",
        f"# seed={seed}",
        f"# prefix_plies={min_plies}..{max_plies}",
        f"# heldout_states_sha256={heldout_states_sha256}",
    ]
    lines.extend(
        f"{opening.opening_id} | {' '.join(opening.moves)}"
        for opening in openings
    )
    return ("\n".join(lines) + "\n").encode("utf-8")


def _write_atomic(path: Path, contents: bytes) -> None:
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=path.parent, prefix=f".{path.name}.",
            suffix=".tmp", delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, path)
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise


def generate_files(
    match_paths: list[Path],
    train_output: Path,
    validation_output: Path,
    manifest_output: Path,
    *,
    train_count: int,
    validation_count: int,
    seed: int,
    min_plies: int = 6,
    max_plies: int = 18,
    max_attempts_per_split: int = 4096,
    overwrite: bool = False,
) -> dict[str, object]:
    destinations = [train_output.resolve(), validation_output.resolve(), manifest_output.resolve()]
    if len(set(destinations)) != len(destinations):
        raise OpeningCorpusError("train, validation, and manifest outputs must be distinct")
    match_sources = {path.resolve() for path in match_paths}
    if any(path in match_sources for path in destinations):
        raise OpeningCorpusError("outputs must not overwrite a held-out match corpus")
    if any(path.exists() for path in destinations) and not overwrite:
        raise OpeningCorpusError("an output file already exists; pass --overwrite to replace it")

    matches: list[Opening] = []
    match_inputs = []
    for path in match_paths:
        parsed = policy_value_selfplay.read_openings(path)
        matches.extend(parsed)
        match_inputs.append({
            "path": _manifest_path(path),
            "opening_count": len(parsed),
            "sha256": _sha256_bytes(path.read_bytes()),
        })
    heldout_states = heldout_position_states(matches)
    heldout_digest = hashlib.sha256(
        "\n".join("\0".join(state) for state in sorted(heldout_states)).encode("ascii")
    ).hexdigest()
    train, validation = generate_opening_splits(
        matches,
        train_count=train_count,
        validation_count=validation_count,
        seed=seed,
        min_plies=min_plies,
        max_plies=max_plies,
        max_attempts_per_split=max_attempts_per_split,
    )
    train_bytes = _serialize_openings(
        "train", train, seed=seed, min_plies=min_plies, max_plies=max_plies,
        heldout_states_sha256=heldout_digest,
    )
    validation_bytes = _serialize_openings(
        "validation", validation, seed=seed, min_plies=min_plies, max_plies=max_plies,
        heldout_states_sha256=heldout_digest,
    )
    _write_atomic(train_output, train_bytes)
    _write_atomic(validation_output, validation_bytes)
    manifest = {
        "schema": "koi-policy-value-openings-v2",
        "generator": "policy_value_openings.py",
        "generator_sha256": _sha256_bytes(Path(__file__).read_bytes()),
        "algorithm": "seeded-legal-random-walk-v1",
        "root_identity": "standard plus canonical FEN fields 1-5; fullmove omitted",
        "seed": seed,
        "prefix_plies": {"minimum": min_plies, "maximum": max_plies},
        "max_attempts_per_split": max_attempts_per_split,
        "heldout_state_count": len(heldout_states),
        "heldout_states_sha256": heldout_digest,
        "match_corpora": match_inputs,
        "train": {"path": _manifest_path(train_output),
                  "opening_count": len(train), "sha256": _sha256_bytes(train_bytes)},
        "validation": {
            "path": _manifest_path(validation_output),
            "opening_count": len(validation), "sha256": _sha256_bytes(validation_bytes),
        },
    }
    _write_atomic(
        manifest_output,
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"),
    )
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--match-corpus", type=Path, nargs="+", default=list(_DEFAULT_MATCH_CORPORA))
    parser.add_argument("--train-output", type=Path, default=_DEFAULT_TRAIN_OUTPUT)
    parser.add_argument("--validation-output", type=Path, default=_DEFAULT_VALIDATION_OUTPUT)
    parser.add_argument("--manifest", type=Path, default=_DEFAULT_MANIFEST)
    parser.add_argument("--train-count", type=int, default=64)
    parser.add_argument("--validation-count", type=int, default=32)
    parser.add_argument("--seed", type=int, default=20260928)
    parser.add_argument("--min-plies", type=int, default=6)
    parser.add_argument("--max-plies", type=int, default=18)
    parser.add_argument("--max-attempts-per-split", type=int, default=4096)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args(argv)
    try:
        manifest = generate_files(
            args.match_corpus, args.train_output, args.validation_output, args.manifest,
            train_count=args.train_count,
            validation_count=args.validation_count,
            seed=args.seed,
            min_plies=args.min_plies,
            max_plies=args.max_plies,
            max_attempts_per_split=args.max_attempts_per_split,
            overwrite=args.overwrite,
        )
    except (OSError, OpeningCorpusError, policy_value_selfplay.SelfPlayError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(json.dumps(manifest, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
