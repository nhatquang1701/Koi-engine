import hashlib
import json
import math
import sys
import tempfile
from types import SimpleNamespace
import unittest
from pathlib import Path

_REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(_REPOSITORY_ROOT))
sys.path.insert(0, str(_REPOSITORY_ROOT / "tools" / "measurement"))

from tools.measurement import policy_value_dataset
from tools.measurement import policy_value_selfplay
import koi_chess as chess
from koi_chess import Move


class FakeEngine:
    def __init__(self, moves):
        self.moves = list(moves)
        self.calls = 0

    def legal_moves(self, board):
        # Deliberately differ from the Python board ordering so the test proves
        # dataset order is taken from the engine's native UCI rules path.
        return list(reversed(list(board.legal_moves)))

    def analyse(self, board, limit):
        if self.calls >= len(self.moves):
            raise AssertionError("fake engine ran out of prepared moves")
        if limit.nodes != 100:
            raise AssertionError("the self-play generator must pass the configured node limit")
        move = Move.from_uci(self.moves[self.calls])
        self.calls += 1
        if move not in board.legal_moves:
            raise AssertionError(f"prepared fake move is illegal: {move}")
        return {"pv": [move]}


class BestMoveOnlyEngine:
    def legal_moves(self, board):
        return list(reversed(list(board.legal_moves)))

    def analyse(self, board, limit):
        return {"pv": []}

    def play(self, board, limit):
        if limit.nodes != 100:
            raise AssertionError("bestmove fallback must use the configured node limit")
        return SimpleNamespace(move=Move.from_uci("e2e4"))


class ScoredFakeEngine(FakeEngine):
    def analyse(self, board, limit):
        result = super().analyse(board, limit)
        result["score"] = chess.engine.PovScore(chess.engine.Cp(200), board.turn)
        return result


class MctsFakeEngine:
    def __init__(self, visits_by_uci):
        self.visits_by_uci = dict(visits_by_uci)
        self.options = {}

    def configure(self, options):
        self.options.update(options)

    def legal_moves(self, board):
        return list(reversed(list(board.legal_moves)))

    def analyse(self, board, limit):
        if limit.nodes != 100:
            raise AssertionError("MCTS self-play must pass the configured node limit")
        return {
            "nodes": sum(self.visits_by_uci.values()),
            "mcts_root_visits": dict(self.visits_by_uci),
        }


class PolicyValueSelfPlayTests(unittest.TestCase):
    def test_training_and_validation_openings_are_separate_from_match_positions(self):
        train_path = _REPOSITORY_ROOT / "tools/measurement/data/policy-value-train-v2.txt"
        validation_path = _REPOSITORY_ROOT / "tools/measurement/data/policy-value-validation-v2.txt"
        match_path = _REPOSITORY_ROOT / "tests/data/openings/openings-curated-32.txt"
        strength_match_path = _REPOSITORY_ROOT / "tests/data/openings/openings-release-strength-160.txt"
        self.assertEqual(policy_value_selfplay._DEFAULT_OPENINGS_PATH, train_path)
        train = policy_value_selfplay.read_openings(train_path)
        validation = policy_value_selfplay.read_openings(validation_path)
        matches = policy_value_selfplay.read_openings(match_path)
        strength_matches = policy_value_selfplay.read_openings(strength_match_path)

        train_ids = {opening.opening_id for opening in train}
        validation_ids = {opening.opening_id for opening in validation}
        match_ids = {opening.opening_id for opening in matches}
        match_ids.update(opening.opening_id for opening in strength_matches)
        self.assertFalse(train_ids & validation_ids)
        self.assertFalse(train_ids & match_ids)
        self.assertFalse(validation_ids & match_ids)

        def root_state(opening):
            board = chess.Board()
            policy_value_selfplay._apply_opening(board, opening)
            return tuple(board.fen().split()[:5])

        def match_prefix_states(openings):
            states = set()
            for opening in openings:
                board = chess.Board()
                for token in opening.moves:
                    move = Move.from_uci(token)
                    self.assertTrue(board.is_legal(move), f"illegal corpus move {token}")
                    board.push(move)
                    states.add(tuple(board.fen().split()[:5]))
            return states

        train_roots = {root_state(opening) for opening in train}
        validation_roots = {root_state(opening) for opening in validation}
        match_states = match_prefix_states([*matches, *strength_matches])
        self.assertFalse(train_roots & validation_roots)
        self.assertFalse(train_roots & match_states)
        self.assertFalse(validation_roots & match_states)

    def test_source_provenance_includes_local_chess_rules_imports(self):
        _, source_hashes = policy_value_selfplay._source_provenance(_REPOSITORY_ROOT)

        for relative_path in (
            "tools/measurement/koi_chess/__init__.py",
            "tools/measurement/koi_chess/attacks.py",
            "tools/measurement/koi_chess/pgn.py",
        ):
            with self.subTest(path=relative_path):
                self.assertEqual(
                    source_hashes[relative_path],
                    hashlib.sha256((_REPOSITORY_ROOT / relative_path).read_bytes()).hexdigest(),
                )

    def test_opening_parser_reads_move_lists_and_rejects_duplicate_ids(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "openings.txt"
            path.write_text(
                "# name | uci moves\n"
                "fools-mate | f2f3 e7e5 g2g4 d8h4\n"
                "bare-start |\n",
                encoding="utf-8",
            )
            openings = policy_value_selfplay.read_openings(path)

            duplicate = Path(temporary_directory) / "duplicate.txt"
            duplicate.write_text("same | e2e4\nsame | d2d4\n", encoding="utf-8")
            with self.assertRaisesRegex(policy_value_selfplay.SelfPlayError, "duplicate opening_id"):
                policy_value_selfplay.read_openings(duplicate)

        self.assertEqual(
            openings,
            [
                policy_value_selfplay.Opening("fools-mate", ("f2f3", "e7e5", "g2g4", "d8h4")),
                policy_value_selfplay.Opening("bare-start", ()),
            ],
        )

    def test_alpha_beta_selfplay_records_legal_one_hot_policy_and_stm_outcome(self):
        fake = FakeEngine(["f2f3", "e7e5", "g2g4", "d8h4"])
        stats = policy_value_selfplay.GenerationStats()
        records = list(policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("fools-mate", ())],
            games=1,
            nodes=100,
            seed=7,
            max_plies=12,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            cpu_variant="generic",
            nnue_sha256="d" * 64,
            source_dirty=True,
            source_sha256={"tools/measurement/policy_value_selfplay.py": "e" * 64},
            stats=stats,
        ))

        self.assertEqual(len(records), 4)
        self.assertEqual(fake.calls, 4)
        self.assertEqual(stats.games_completed, 1)
        self.assertEqual(stats.records_written, 4)
        self.assertEqual(stats.outcome_counts, {"black-win": 1})
        self.assertEqual(records[-1]["termination_reason"], "checkmate")
        self.assertEqual([record["outcome"] for record in records], ["loss", "win", "loss", "win"])
        self.assertEqual([record["value_target"] for record in records], [-1.0, 1.0, -1.0, 1.0])

        for record, expected_move in zip(records, fake.moves):
            policy_value_dataset.validate_record(record)
            actions = record["legal_actions"]
            board = chess.Board(record["position"]["fen"])
            native_order = [move.uci() for move in reversed(list(board.legal_moves))]
            self.assertEqual([action["uci"] for action in actions], native_order)
            self.assertEqual(len(actions), len(set(action["uci"] for action in actions)))
            self.assertEqual(sum(record["policy_targets"]), 1.0)
            self.assertEqual(
                [action["uci"] for action, target in zip(actions, record["policy_targets"]) if target],
                [expected_move],
            )
            self.assertNotIn("visit_counts", record)
            self.assertIsNone(record["network_sha256"])
            self.assertEqual(record["producer"]["kind"], "alpha-beta-distillation")
            self.assertEqual(record["search_provenance"]["algorithm"], "AlphaBeta")
            self.assertEqual(record["search_provenance"]["options"]["cpu_variant"], "generic")
            self.assertEqual(record["search_provenance"]["options"]["nnue_sha256"], "d" * 64)
            self.assertTrue(record["search_provenance"]["options"]["source_dirty"])
            self.assertEqual(
                record["search_provenance"]["options"]["source_sha256"],
                {"tools/measurement/policy_value_selfplay.py": "e" * 64},
            )
            self.assertEqual(record["opening_id"], "fools-mate")
            self.assertEqual(record["seed"], 7)
        self.assertEqual([record["ply"] for record in records], [0, 1, 2, 3])

    def test_maximum_ply_cutoff_is_an_explicit_draw(self):
        fake = FakeEngine(["e2e4", "e7e5"])
        stats = policy_value_selfplay.GenerationStats()
        records = list(policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("short-opening", ())],
            games=1,
            nodes=100,
            seed=11,
            max_plies=2,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            stats=stats,
        ))

        self.assertEqual(len(records), 2)
        self.assertEqual({record["termination_reason"] for record in records}, {"ply-limit"})
        self.assertEqual({record["outcome"] for record in records}, {"draw"})
        self.assertEqual({record["value_target"] for record in records}, {0.0})
        self.assertEqual(stats.outcome_counts, {"draw": 1})
        self.assertEqual(stats.termination_counts, {"ply-limit": 1})

    def test_bestmove_is_used_when_a_node_limited_search_has_no_info_pv(self):
        stats = policy_value_selfplay.GenerationStats()
        records = list(policy_value_selfplay.generate_records(
            BestMoveOnlyEngine(),
            [policy_value_selfplay.Opening("short-opening", ())],
            games=1,
            nodes=100,
            seed=5,
            max_plies=1,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            stats=stats,
        ))

        self.assertEqual(len(records), 1)
        self.assertEqual(
            [action["uci"] for action, target in zip(
                records[0]["legal_actions"], records[0]["policy_targets"]
            ) if target],
            ["e2e4"],
        )
        self.assertEqual(
            records[0]["search_provenance"]["options"]["policy_target_source"],
            "bestmove-fallback",
        )
        self.assertEqual(stats.bestmove_fallbacks, 1)

    def test_alpha_beta_score_is_distilled_separately_from_game_outcome(self):
        fake = ScoredFakeEngine(["e2e4"])
        records = list(policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("score-fixture", ())],
            games=1,
            nodes=100,
            seed=13,
            max_plies=1,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
        ))

        self.assertEqual(records[0]["outcome"], "draw")
        self.assertAlmostEqual(records[0]["value_target"], math.tanh(0.5), places=7)
        self.assertEqual(
            records[0]["search_provenance"]["options"]["value_target_source"],
            "alphabeta-score",
        )

    def test_mcts_selfplay_records_visit_targets_and_uses_them_for_move_selection(self):
        board = chess.Board()
        native_moves = [move.uci() for move in reversed(list(board.legal_moves))]
        visits_by_uci = {move: 0 for move in native_moves}
        visits_by_uci[native_moves[0]] = 7
        visits_by_uci[native_moves[1]] = 3
        fake = MctsFakeEngine(visits_by_uci)
        records = list(policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("mcts-opening", ())],
            games=1,
            nodes=100,
            seed=19,
            max_plies=1,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            search_algorithm="MCTS",
            policy_value_file="model.kpv",
            policy_value_sha256="f" * 64,
            temperature=0.0,
        ))

        record = records[0]
        policy_value_dataset.validate_record(record)
        self.assertEqual(record["search_provenance"]["algorithm"], "MCTS")
        self.assertEqual(record["producer"]["kind"], "mcts-self-play")
        self.assertEqual(record["network_sha256"], "f" * 64)
        self.assertEqual(record["visit_counts"], [visits_by_uci[action["uci"]] for action in record["legal_actions"]])
        self.assertEqual(record["policy_targets"], [count / 10 for count in record["visit_counts"]])
        self.assertEqual(record["search_provenance"]["options"]["policy_target_source"], "mcts-root-visits")
        self.assertEqual(record["search_provenance"]["options"]["sampled_move"], native_moves[0])
        self.assertEqual(record["search_provenance"]["options"]["move_sampling_seed"], 20)
        self.assertEqual(fake.options["SearchAlgorithm"], "MCTS")
        self.assertEqual(fake.options["PolicyValueFile"], "model.kpv")
        self.assertTrue(fake.options["MCTSVisitOutput"])
        self.assertTrue(fake.options["MCTSSelfPlay"])
        self.assertEqual(fake.options["RandomSeed"], 19)
        self.assertEqual(
            record["search_provenance"]["options"]["mcts_root_noise_seed"], 19
        )
        self.assertEqual(
            record["search_provenance"]["options"]["mcts_root_noise_alpha"], 0.3
        )
        self.assertEqual(
            record["search_provenance"]["options"]["mcts_root_noise_epsilon"], 0.25
        )
        self.assertFalse(fake.options["OwnBook"])
        self.assertEqual(fake.options["Threads"], 1)

    def test_mcts_selfplay_rejects_incomplete_visit_maps(self):
        fake = MctsFakeEngine({"e2e4": 1})
        with self.assertRaisesRegex(policy_value_selfplay.SelfPlayError, "complete legal action set"):
            list(policy_value_selfplay.generate_records(
                fake,
                [policy_value_selfplay.Opening("mcts-opening", ())],
                games=1,
                nodes=100,
                seed=19,
                max_plies=1,
                engine_sha256="a" * 64,
                opening_corpus_sha256="b" * 64,
                source_revision="c" * 40,
                engine_version="test-build",
                search_algorithm="MCTS",
                policy_value_file="model.kpv",
                policy_value_sha256="f" * 64,
            ))

    def test_mcts_move_sampling_switches_to_argmax_after_temperature_plies(self):
        board = chess.Board()
        board.push(Move.from_uci("e2e4"))
        native_moves = [move.uci() for move in reversed(list(board.legal_moves))]
        fake = MctsFakeEngine({move: 1 for move in native_moves})
        records = list(policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("late-temperature", ("e2e4",))],
            games=1,
            nodes=100,
            seed=29,
            max_plies=2,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            search_algorithm="MCTS",
            policy_value_file="model.kpv",
            policy_value_sha256="f" * 64,
            temperature=1.0,
            temperature_plies=1,
        ))

        record = records[0]
        options = record["search_provenance"]["options"]
        self.assertEqual(record["ply"], 1)
        self.assertEqual(options["sampling_temperature"], 0.0)
        self.assertEqual(options["sampled_move"], native_moves[0])

    def test_mcts_move_sampling_records_temperature_before_cutoff(self):
        board = chess.Board()
        native_moves = [move.uci() for move in reversed(list(board.legal_moves))]
        visits = {move: 0 for move in native_moves}
        visits[native_moves[0]] = 8
        visits[native_moves[1]] = 2
        records = list(policy_value_selfplay.generate_records(
            MctsFakeEngine(visits),
            [policy_value_selfplay.Opening("early-temperature", ())],
            games=1,
            nodes=100,
            seed=31,
            max_plies=1,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            search_algorithm="MCTS",
            policy_value_file="model.kpv",
            policy_value_sha256="f" * 64,
            temperature=1.0,
            temperature_plies=1,
        ))

        options = records[0]["search_provenance"]["options"]
        self.assertEqual(options["sampling_temperature"], 1.0)
        self.assertEqual(options["policy_temperature_plies"], 1)

    def test_mcts_temperature_plies_must_be_non_negative_integer(self):
        fake = MctsFakeEngine({"e2e4": 1})
        with self.assertRaisesRegex(
            policy_value_selfplay.SelfPlayError,
            "temperature_plies must be a non-negative integer",
        ):
            list(policy_value_selfplay.generate_records(
                fake,
                [policy_value_selfplay.Opening("invalid-temperature", ())],
                games=1,
                nodes=100,
                seed=37,
                max_plies=1,
                engine_sha256="a" * 64,
                opening_corpus_sha256="b" * 64,
                source_revision="c" * 40,
                engine_version="test-build",
                search_algorithm="MCTS",
                policy_value_file="model.kpv",
                policy_value_sha256="f" * 64,
                temperature_plies=-1,
            ))

    def test_dataset_and_manifest_are_written_with_stable_hashes(self):
        fake = FakeEngine(["f2f3", "e7e5", "g2g4", "d8h4"])
        stats = policy_value_selfplay.GenerationStats()
        records = policy_value_selfplay.generate_records(
            fake,
            [policy_value_selfplay.Opening("fools-mate", ())],
            games=1,
            nodes=100,
            seed=7,
            max_plies=12,
            engine_sha256="a" * 64,
            opening_corpus_sha256="b" * 64,
            source_revision="c" * 40,
            engine_version="test-build",
            stats=stats,
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "dataset.jsonl"
            manifest = Path(temporary_directory) / "dataset.manifest.json"
            manifest_fields = {"seed": 7, "games": 1}

            def with_generation_stats():
                yield from records
                manifest_fields.update({
                    "games_completed": stats.games_completed,
                    "records_written": stats.records_written,
                    "outcome_counts": stats.outcome_counts,
                    "termination_counts": stats.termination_counts,
                    "selected_openings": stats.selected_openings,
                })

            result = policy_value_selfplay.write_dataset(
                output, manifest, with_generation_stats(), manifest_fields
            )
            read_back = list(policy_value_dataset.read_jsonl(output))
            manifest_data = json.loads(manifest.read_text(encoding="utf-8"))

            self.assertEqual(len(read_back), 4)
            self.assertEqual(read_back[-1]["termination_reason"], "checkmate")
            self.assertEqual(result["dataset_sha256"], hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(manifest_data["dataset_sha256"], result["dataset_sha256"])
            self.assertEqual(manifest_data["record_count"], 4)
            self.assertEqual(manifest_data["games_completed"], 1)
            self.assertEqual(manifest_data["records_written"], 4)
            with self.assertRaisesRegex(policy_value_selfplay.SelfPlayError, "already exists"):
                policy_value_selfplay.write_dataset(
                    output, manifest, records, {"seed": 7, "games": 1}
                )


if __name__ == "__main__":
    unittest.main()
