import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

_REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_REPOSITORY_ROOT))
sys.path.insert(0, str(_REPOSITORY_ROOT / "tools" / "measurement"))

from tools.measurement import policy_value_openings
from tools.measurement import policy_value_selfplay
import koi_chess as chess
from koi_chess import Move


class PolicyValueOpeningGenerationTests(unittest.TestCase):
    def test_generation_is_repeatable_legal_and_disjoint_from_heldout_prefixes(self):
        with tempfile.TemporaryDirectory() as directory:
            match_corpus = Path(directory) / "heldout.txt"
            match_corpus.write_text(
                "match-ruy | e2e4 e7e5 g1f3 b8c6 f1b5\n"
                "match-queen-pawn | d2d4 d7d5 c1f4 g8f6 e2e3\n",
                encoding="utf-8",
            )
            matches = policy_value_selfplay.read_openings(match_corpus)
            kwargs = {
                "train_count": 5,
                "validation_count": 3,
                "seed": 20260928,
                "min_plies": 4,
                "max_plies": 8,
                "max_attempts_per_split": 128,
            }
            train, validation = policy_value_openings.generate_opening_splits(
                matches, **kwargs
            )
            repeated = policy_value_openings.generate_opening_splits(matches, **kwargs)

        self.assertEqual((train, validation), repeated)
        self.assertEqual(len(train), 5)
        self.assertEqual(len(validation), 3)
        all_openings = [*train, *validation]
        all_ids = [opening.opening_id for opening in all_openings]
        self.assertEqual(len(all_ids), len(set(all_ids)))

        roots = []
        for opening in all_openings:
            self.assertGreaterEqual(len(opening.moves), 4)
            self.assertLessEqual(len(opening.moves), 8)
            board = chess.Board()
            for token in opening.moves:
                move = Move.from_uci(token)
                self.assertTrue(board.is_legal(move), f"illegal generated move {token}")
                board.push(move)
            roots.append(policy_value_openings.position_state(board))

        self.assertEqual(len(roots), len(set(roots)), "all split roots must be unique")
        heldout_states = policy_value_openings.heldout_position_states(matches)
        self.assertIn(
            policy_value_openings.position_state(chess.Board()), heldout_states,
            "the held-out set must include the initial board as well as played prefixes",
        )
        self.assertFalse(set(roots) & heldout_states)

    def test_generation_fails_when_all_requested_roots_are_held_out(self):
        start = chess.Board()
        matches = [
            policy_value_selfplay.Opening(f"heldout-{move.uci()}", (move.uci(),))
            for move in sorted(start.legal_moves, key=lambda move: move.uci())
        ]
        with self.assertRaisesRegex(policy_value_openings.OpeningCorpusError, "could not generate"):
            policy_value_openings.generate_opening_splits(
                matches,
                train_count=1,
                validation_count=1,
                seed=4,
                min_plies=1,
                max_plies=1,
                max_attempts_per_split=32,
            )

    def test_generation_writes_reproducible_corpora_and_manifest_outside_repo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            match_corpus = root / "match.txt"
            match_corpus.write_text("heldout | e2e4 e7e5 g1f3\n", encoding="utf-8")
            train = root / "train.txt"
            validation = root / "validation.txt"
            manifest = root / "manifest.json"
            arguments = {
                "train_count": 3,
                "validation_count": 2,
                "seed": 91,
                "min_plies": 4,
                "max_plies": 7,
                "max_attempts_per_split": 128,
            }
            generated = policy_value_openings.generate_files(
                [match_corpus], train, validation, manifest, **arguments
            )
            first_bytes = (train.read_bytes(), validation.read_bytes(), manifest.read_bytes())
            generated_again = policy_value_openings.generate_files(
                [match_corpus], train, validation, manifest,
                overwrite=True, **arguments,
            )

            self.assertEqual(generated, generated_again)
            self.assertEqual(first_bytes, (train.read_bytes(), validation.read_bytes(), manifest.read_bytes()))
            self.assertEqual(len(policy_value_selfplay.read_openings(train)), 3)
            self.assertEqual(len(policy_value_selfplay.read_openings(validation)), 2)
            parsed = json.loads(manifest.read_text(encoding="utf-8"))
            self.assertEqual(parsed["train"]["sha256"], hashlib.sha256(train.read_bytes()).hexdigest())
            self.assertEqual(
                parsed["validation"]["sha256"],
                hashlib.sha256(validation.read_bytes()).hexdigest(),
            )
            self.assertEqual(
                parsed["generator_sha256"],
                hashlib.sha256(Path(policy_value_openings.__file__).read_bytes()).hexdigest(),
            )

    def test_generation_refuses_to_overwrite_a_heldout_match_corpus(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            match_corpus = root / "match.txt"
            match_corpus.write_text("heldout | e2e4 e7e5 g1f3\n", encoding="utf-8")
            with self.assertRaisesRegex(policy_value_openings.OpeningCorpusError, "held-out match"):
                policy_value_openings.generate_files(
                    [match_corpus],
                    match_corpus,
                    root / "validation.txt",
                    root / "manifest.json",
                    train_count=1,
                    validation_count=1,
                    seed=3,
                    min_plies=4,
                    max_plies=5,
                    max_attempts_per_split=32,
                    overwrite=True,
                )


if __name__ == "__main__":
    unittest.main()
