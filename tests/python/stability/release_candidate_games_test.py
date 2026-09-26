"""Tests for the standalone release-candidate Cute Chess artifact gate."""

from __future__ import annotations

import json
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "stability"))
from validate_release_candidate_games import chess, main, validate_match_artifacts  # noqa: E402


class ReleaseCandidateGamesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = pathlib.Path(self.temp.name)
        self.openings = self.directory / "openings.txt"
        self.openings.write_text("test-opening | e2e4\n", encoding="utf-8")
        self.pgn = self.directory / "games.pgn"
        self.report = self.directory / "report.json"
        self.write_report()
        # The PGN root is the curated opening position after e2e4. The game
        # then plays two legal moves and records a finished draw.
        self.pgn.write_text(
            '[Event "release candidate"]\n'
            '[White "Koi"]\n'
            '[Black "Stockfish"]\n'
            '[Result "1/2-1/2"]\n'
            '[SetUp "1"]\n'
            '[FEN "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"]\n\n'
            "1... c5 2. Nf3 1/2-1/2\n\n",
            encoding="utf-8",
        )

    def write_report(self, *, exit_code=0, termination="completed", failures=0,
                     started=1, finished=1, color="white", time_control="1+0",
                     threads=1, max_moves=60, own_book=False,
                     koi_white=1, koi_black=0):
        data = {
            "schema": "koi-cutechess-stability-v1",
            "configuration": {
                "games": 1,
                "koi_color": color,
                "time_control": time_control,
                "threads": threads,
                "max_moves": max_moves,
                "own_book": own_book,
            },
            "results": {
                "exit_code": exit_code,
                "termination_classification": termination,
                "started_games": started,
                "finished_games": finished,
                "failures": failures,
                "failure_lines": [],
                "koi_white": koi_white,
                "koi_black": koi_black,
            },
        }
        self.report.write_text(json.dumps(data), encoding="utf-8")

    def write_knight_shuffle_pgn(self, opening_moves: tuple[str, ...], engine_plies: int):
        self.openings.write_text(
            "test-opening | " + " ".join(opening_moves) + "\n", encoding="utf-8"
        )
        board = chess.Board()
        for move_text in opening_moves:
            board.push(chess.Move.from_uci(move_text))
        initial_fen = board.fen()
        cycle = (
            ("g1f3", "g8f6", "f3g1", "f6g8") if board.turn else
            ("g8f6", "g1f3", "f6g8", "f3g1")
        )
        tokens = []
        for index in range(engine_plies):
            move = chess.Move.from_uci(cycle[index % len(cycle)])
            self.assertTrue(board.is_legal(move))
            tokens.append(f"{board.fullmove_number}{'.' if board.turn else '...'} {board.san(move)}")
            board.push(move)
        self.pgn.write_text(
            '[Event "release candidate"]\n[White "Koi"]\n[Black "Stockfish"]\n'
            '[Result "1/2-1/2"]\n[Termination "adjudication"]\n[SetUp "1"]\n'
            f'[FEN "{initial_fen}"]\n\n' + " ".join(tokens) + " 1/2-1/2\n",
            encoding="utf-8",
        )

    def validate(self, **overrides):
        options = {
            "expected_games": 1,
            "expected_color": "white",
            "expected_time_control": "1+0",
            "expected_threads": 1,
            "expected_max_moves": 60,
            "expected_opening_count": 1,
        }
        options.update(overrides)
        return validate_match_artifacts(self.pgn, self.report, self.openings, **options)

    def write_evaluator_attestation(
        self,
        *,
        mode="nnue-v4",
        sha256="a" * 64,
        path="C:/nets/fixture.nnue",
        confirmation="info string NNUE enabled from C:/nets/fixture.nnue",
        rejections=(),
        gpu_marker=None,
        gpu_fallbacks=(),
        gpu_requested=None,
        threads=1,
    ):
        self.write_uci_match()
        data = json.loads(self.report.read_text(encoding="utf-8"))
        configuration = data["configuration"]
        configuration.update({
            "threads": threads,
            "koi_evaluator_mode": mode,
            "koi_evalfile_path": path,
            "koi_evalfile_sha256": sha256,
            "koi_gpu_nnue_requested": mode == "gpu-v5" if gpu_requested is None else gpu_requested,
            "koi_evaluator_attestation": {
                "nnue_enabled_confirmation": confirmation,
                "evalfile_rejections": list(rejections),
                "gpu_enabled_marker": gpu_marker,
                "gpu_unavailable_fallbacks": list(gpu_fallbacks),
            },
        })
        self.report.write_text(json.dumps(data), encoding="utf-8")

    def test_accepts_a_complete_legal_match_bundle(self):
        self.assertEqual(self.validate(), [])

    def test_rejects_process_failures_and_configuration_drift(self):
        self.write_report(exit_code=1, termination="crash", failures=1, own_book=True)
        errors = self.validate()
        self.assertTrue(any("exit code" in error for error in errors))
        self.assertTrue(any("termination" in error for error in errors))
        self.assertTrue(any("failures" in error for error in errors))
        self.assertTrue(any("own_book" in error for error in errors))

    def test_rejects_illegal_or_unfinished_games(self):
        self.pgn.write_text(
            '[Result "*"]\n\n1. e5 *\n', encoding="utf-8"
        )
        errors = self.validate()
        self.assertTrue(any("illegal" in error.lower() for error in errors))
        self.assertTrue(any("unfinished" in error.lower() or "result" in error.lower() for error in errors))

    def test_rejects_cute_chess_result_without_engine_play(self):
        self.pgn.write_text(
            '[Event "release candidate"]\n[White "Koi"]\n[Black "Stockfish"]\n'
            '[Result "1/2-1/2"]\n[SetUp "1"]\n'
            '[FEN "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"]\n\n'
            '1/2-1/2\n',
            encoding="utf-8",
        )
        self.assertTrue(any("no engine moves" in error for error in self.validate()))

    def test_rejects_a_match_missing_curated_opening_coverage(self):
        self.openings.write_text("other-opening | d2d4\n", encoding="utf-8")
        errors = self.validate()
        self.assertTrue(any("opening" in error.lower() for error in errors))

    def test_max_moves_is_a_full_move_cap(self):
        # The two plies after the EPD opening are one Cute Chess full move.
        self.write_report(max_moves=1)
        self.assertEqual(self.validate(expected_max_moves=1), [])

    def test_black_to_move_opening_allows_one_delayed_cap_ply(self):
        self.write_report(max_moves=30)
        self.write_knight_shuffle_pgn(("e2e4",), 61)
        self.assertEqual(self.validate(expected_max_moves=30), [])

    def test_full_move_cap_allows_one_delay_but_rejects_two_for_either_side(self):
        self.write_report(max_moves=30)
        self.write_knight_shuffle_pgn(("e2e4",), 62)
        self.assertTrue(any("at most 61" in error for error in self.validate(expected_max_moves=30)))
        self.write_knight_shuffle_pgn(("e2e4", "e7e5"), 61)
        self.assertEqual(self.validate(expected_max_moves=30), [])
        self.write_knight_shuffle_pgn(("e2e4", "e7e5"), 62)
        self.assertTrue(any("at most 61" in error for error in self.validate(expected_max_moves=30)))

    def test_rejects_actual_color_counts_that_disagree_with_requested_color(self):
        self.write_report(koi_white=0, koi_black=1)
        errors = self.validate()
        self.assertTrue(any("actual Koi color" in error for error in errors))

    def test_opening_coverage_can_be_disabled_for_a_small_pilot(self):
        self.assertEqual(self.validate(expected_opening_count=0), [])

    def test_cli_labels_the_cute_chess_limit_as_full_moves(self):
        self.write_report(max_moves=1)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = main([
                "--pgn", str(self.pgn), "--report", str(self.report),
                "--openings", str(self.openings), "--expected-games", "1",
                "--expected-color", "white", "--expected-time-control", "1+0",
                "--expected-threads", "1", "--expected-max-moves", "1",
                "--expected-opening-count", "1",
            ])
        self.assertEqual(result, 0)
        self.assertIn("max full moves=1", output.getvalue())
        self.assertIn("up to 3 plies with one cap-check delay", output.getvalue())

    def write_uci_match(self, *, termination="max plies", result="*",
                         process="clean shutdown", bestmove="bestmove e2e4"):
        self.pgn.write_text(
            '[Event "Koi Engine UCI match"]\n'
            '[White "Koi"]\n'
            '[Black "Opponent"]\n'
            '[Result "*"]\n'
            '[MoveFormat "UCI coordinate notation"]\n'
            '[TimeControl "1+0"]\n\n'
            "1. e2e4 *\n",
            encoding="utf-8",
        )
        game = {
            "position": "test-opening",
            "initial_fen": "startpos",
            "koi_color": "white",
            "result": result,
            "termination": termination,
            "process_status": {"koi": process, "opponent": "clean shutdown"},
            "moves": [{
                "ply": 1,
                "side": "w",
                "move": "e2e4",
                "replay_legal": True,
                "bestmove_line": bestmove,
            }],
        }
        report = {
            "schema": "koi-uci-match-v2",
            "configuration": {
                "max_plies": 1,
                "koi_color": "white",
                "time_control": "1+0",
                "threads": 1,
                "koi_own_book": False,
            },
            "games": [game],
        }
        self.report.write_text(json.dumps(report), encoding="utf-8")

    def test_accepts_max_plies_as_a_completed_bounded_uci_game(self):
        self.write_uci_match()
        self.assertEqual(
            self.validate(expected_max_moves=1, expected_opening_count=0), []
        )

    def test_accepts_attested_nnue_mode_when_expected_hash_matches(self):
        expected_hash = "a" * 64
        self.write_evaluator_attestation(mode="nnue-v4", sha256=expected_hash)
        self.assertEqual(
            self.validate(
                expected_max_moves=1,
                expected_opening_count=0,
                expected_evaluator_mode="nnue-v4",
                expected_evalfile_sha256=expected_hash,
            ),
            [],
        )

    def test_rejects_missing_or_rejected_nnue_attestation(self):
        expected_hash = "a" * 64
        cases = (
            {"confirmation": None},
            {"rejections": ["info string EvalFile rejected: checksum mismatch"]},
            {"sha256": "b" * 64},
            {"mode": "nnue-v5"},
        )
        for override in cases:
            with self.subTest(override=override):
                self.write_evaluator_attestation(**override)
                errors = self.validate(
                    expected_max_moves=1,
                    expected_opening_count=0,
                    expected_evaluator_mode="nnue-v4",
                    expected_evalfile_sha256=expected_hash,
                )
                self.assertTrue(errors)

    def test_gpu_v5_requires_requested_gpu_marker_threads_and_no_fallback(self):
        expected_hash = "c" * 64
        valid = {
            "mode": "gpu-v5",
            "sha256": expected_hash,
            "confirmation": "info string NNUE enabled from C:/nets/fixture.nnue",
            "gpu_marker": "koi-engine: GPU NNUE inference enabled.",
            "gpu_requested": True,
            "threads": 2,
        }
        self.write_evaluator_attestation(**valid)
        self.assertEqual(
            self.validate(
                expected_max_moves=1,
                expected_threads=2,
                expected_opening_count=0,
                expected_evaluator_mode="gpu-v5",
                expected_evalfile_sha256=expected_hash,
            ),
            [],
        )

        invalid_cases = (
            {"gpu_marker": None},
            {"gpu_fallbacks": ["koi-engine: GPU NNUE unavailable (no device); using the CPU network."]},
            {"gpu_requested": False},
            {"threads": 1},
        )
        for override in invalid_cases:
            with self.subTest(override=override):
                self.write_evaluator_attestation(**(valid | override))
                errors = self.validate(
                    expected_max_moves=1,
                    expected_threads=2,
                    expected_opening_count=0,
                    expected_evaluator_mode="gpu-v5",
                    expected_evalfile_sha256=expected_hash,
                )
                self.assertTrue(errors)

    def test_cli_accepts_explicit_evaluator_mode_and_hash(self):
        expected_hash = "d" * 64
        self.write_evaluator_attestation(mode="nnue-v5", sha256=expected_hash)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = main([
                "--pgn", str(self.pgn), "--report", str(self.report),
                "--openings", str(self.openings), "--expected-games", "1",
                "--expected-color", "white", "--expected-time-control", "1+0",
                "--expected-threads", "1", "--expected-max-moves", "1",
                "--expected-opening-count", "0", "--expected-evaluator-mode", "nnue-v5",
                "--expected-evalfile-sha256", expected_hash,
            ])
        self.assertEqual(result, 0)
        self.assertIn("evaluator=nnue-v5", output.getvalue())
        self.assertIn(f"EvalFile SHA-256={expected_hash}", output.getvalue())

    def test_accepts_harness_rule_draw_termination(self):
        self.write_uci_match(termination="rule draw", result="1/2-1/2")
        self.pgn.write_text(
            self.pgn.read_text(encoding="utf-8").replace('[Result "*"]', '[Result "1/2-1/2"]').replace("e2e4 *", "e2e4 1/2-1/2"),
            encoding="utf-8",
        )
        self.assertEqual(
            self.validate(expected_max_moves=1, expected_opening_count=0), []
        )

    def test_rejects_uci_protocol_and_process_failures(self):
        self.write_uci_match(process="crashed", bestmove="bestmove e2e4 bestmove e2e4")
        errors = self.validate(expected_max_moves=1, expected_opening_count=0)
        self.assertTrue(any("process" in error.lower() for error in errors))
        self.assertTrue(any("bestmove" in error.lower() for error in errors))

    def test_rejects_uci_result_without_played_moves(self):
        self.write_uci_match(termination="rule draw", result="1/2-1/2")
        data = json.loads(self.report.read_text(encoding="utf-8"))
        data["games"][0]["moves"] = []
        self.report.write_text(json.dumps(data), encoding="utf-8")
        self.pgn.write_text(
            '[Event "Koi Engine UCI match"]\n[Result "1/2-1/2"]\n'
            '[MoveFormat "UCI coordinate notation"]\n\n1/2-1/2\n',
            encoding="utf-8",
        )
        errors = self.validate(expected_max_moves=1, expected_opening_count=0)
        self.assertTrue(any("no engine moves" in error for error in errors))

    def test_rejects_uci_opening_only_result(self):
        self.write_uci_match(termination="rule draw", result="1/2-1/2")
        data = json.loads(self.report.read_text(encoding="utf-8"))
        data["games"][0]["moves"][0]["engine_label"] = "opening"
        data["games"][0]["moves"][0]["bestmove_line"] = None
        self.report.write_text(json.dumps(data), encoding="utf-8")
        self.pgn.write_text(
            self.pgn.read_text(encoding="utf-8")
            .replace('[Result "*"]', '[Result "1/2-1/2"]')
            .replace("e2e4 *", "e2e4 1/2-1/2"),
            encoding="utf-8",
        )
        errors = self.validate(expected_max_moves=1, expected_opening_count=1)
        self.assertTrue(any("no engine moves" in error for error in errors))

    def test_rejects_uci_missing_opponent_process_status(self):
        self.write_uci_match()
        data = json.loads(self.report.read_text(encoding="utf-8"))
        data["games"][0]["process_status"] = {"koi": "clean shutdown"}
        self.report.write_text(json.dumps(data), encoding="utf-8")
        errors = self.validate(expected_max_moves=1, expected_opening_count=0)
        self.assertTrue(any("opponent" in error.lower() and "status" in error.lower() for error in errors))

    def test_max_plies_cap_starts_after_the_curated_opening(self):
        self.write_uci_match()
        self.pgn.write_text(
            self.pgn.read_text(encoding="utf-8").replace("1. e2e4 *", "1. e2e4 c7c5 *"),
            encoding="utf-8",
        )
        report = json.loads(self.report.read_text(encoding="utf-8"))
        # The match harness injects curated opening moves without asking an
        # engine for bestmove; only subsequent plies have a UCI response.
        report["games"][0]["moves"][0]["engine_label"] = "opening"
        report["games"][0]["moves"][0]["bestmove_line"] = None
        report["games"][0]["moves"].append({
            "ply": 2,
            "side": "b",
            "move": "c7c5",
            "engine_label": "Opponent",
            "replay_legal": True,
            "bestmove_line": "bestmove c7c5",
        })
        self.report.write_text(json.dumps(report), encoding="utf-8")
        self.assertEqual(self.validate(expected_max_moves=1, expected_opening_count=1), [])

    def test_short_opening_is_not_mistaken_for_a_longer_matching_prefix(self):
        self.write_uci_match()
        self.openings.write_text(
            "long-opening | e2e4 c7c5\nshort-opening | e2e4\n",
            encoding="utf-8",
        )
        self.pgn.write_text(
            self.pgn.read_text(encoding="utf-8").replace("1. e2e4 *", "1. e2e4 c7c5 *"),
            encoding="utf-8",
        )
        report = json.loads(self.report.read_text(encoding="utf-8"))
        game = report["games"][0]
        game["position"] = "short-opening"
        game["moves"][0]["engine_label"] = "opening"
        game["moves"][0]["bestmove_line"] = None
        game["moves"].append({
            "ply": 2, "side": "b", "move": "c7c5", "engine_label": "Opponent",
            "replay_legal": True, "bestmove_line": "bestmove c7c5",
        })
        self.report.write_text(json.dumps(report), encoding="utf-8")
        errors = self.validate(expected_max_moves=1, expected_opening_count=2)
        self.assertEqual(errors, ["report does not cover curated openings: long-opening"])


if __name__ == "__main__":
    unittest.main()
