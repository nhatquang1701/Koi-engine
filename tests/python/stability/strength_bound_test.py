import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "tools" / "stability" / "strength_bound.py"
CANDIDATE = "a" * 64
BASELINE = "b" * 64
EXPECTED = {"nodes": 1000, "threads": 1, "hash_mb": 64, "speed": 100, "max_plies": 80}


def match(koi_color: str, results: list[str], *, candidate_hash: str = CANDIDATE) -> dict:
    games = []
    for index, result in enumerate(results):
        games.append({
            "position": f"opening-{index}", "initial_fen": "startpos", "round": 1,
            "koi_color": koi_color, "result": result,
            "termination": "max plies" if result == "*" else "checkmate",
            "process_status": {"koi": "clean shutdown", "opponent": "clean shutdown"},
            "moves": [
                {"move": "e2e4", "replay_legal": True},
                {"move": "e7e5", "replay_legal": True},
            ],
        })
    return {
        "schema": "koi-uci-match-v2",
        "configuration": {
            "nodes": 1000, "threads": 1, "hash_mb": 64, "speed": 100,
            "max_plies": 80, "koi_color": koi_color, "koi_own_book": False,
            "koi_book_random": False, "opponent_limit_strength": False,
        },
        "measurement": {"book": {"enabled": False}, "tablebase": {"enabled": False}},
        "engines": [
            {"label": "Koi", "process_status": "clean shutdown", "exit_code": 0,
             "hashes": {"executable_sha256": candidate_hash},
             "handshake": [
                 "option name RandomSeed type spin default 0 min 0 max 2147483647",
                 "option name Hash type spin default 512 min 1 max 4096",
                 "option name Threads type spin default 1 min 1 max 8",
                 "option name Speed type spin default 100 min 1 max 100",
                 "option name OwnBook type check default true",
                 "option name BookFile type string default book.bin",
                 "option name BookDepth type spin default 16 min 0 max 40",
                 "option name BookRandom type check default false",
             ],
             "options": ["setoption name Hash value 64", "setoption name Threads value 1"]},
            {"label": "Opponent", "process_status": "clean shutdown", "exit_code": 0,
             "hashes": {"executable_sha256": BASELINE},
             "handshake": [
                 "option name RandomSeed type spin default 0 min 0 max 2147483647",
                 "option name Hash type spin default 512 min 1 max 4096",
                 "option name Threads type spin default 1 min 1 max 8",
                 "option name Speed type spin default 100 min 1 max 100",
                 "option name OwnBook type check default true",
                 "option name BookFile type string default book.bin",
                 "option name BookDepth type spin default 16 min 0 max 40",
                 "option name BookRandom type check default false",
             ],
             "options": ["setoption name Hash value 64", "setoption name Threads value 1"]},
        ],
        "positions": [{"Name": f"opening-{i}", "Fen": "startpos", "Moves": ["e2e4"]}
                      for i in range(len(results))],
        "games": games,
    }


def write_match(directory: Path, color: str, results: list[str], **kwargs) -> Path:
    path = directory / f"{color}.json"
    path.write_text(json.dumps(match(color, results, **kwargs)), encoding="utf-8")
    games = "\n\n".join(
        f'[Event "match"]\n[Result "{result}"]\n[MoveFormat "UCI coordinate notation"]\n\n1. e2e4 e7e5 {result}'
        for result in results
    )
    path.with_suffix(".pgn").write_text(games, encoding="utf-8")
    return path


def invoke(white: Path, black: Path, output: Path, *, candidate_hash: str = CANDIDATE) -> subprocess.CompletedProcess[str]:
    return subprocess.run([
        sys.executable, str(SCRIPT), "--white", str(white), "--black", str(black),
        "--candidate-sha256", candidate_hash, "--baseline-sha256", BASELINE,
        "--expected-config", json.dumps(EXPECTED), "--output", str(output),
    ], capture_output=True, text=True, check=False)


class StrengthBoundTests(unittest.TestCase):
    def test_result_headers_without_played_moves_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1/2-1/2"])
            black = write_match(root, "black", ["1/2-1/2"])
            for path in (white, black):
                data = json.loads(path.read_text(encoding="utf-8"))
                data["games"][0]["moves"] = []
                path.write_text(json.dumps(data), encoding="utf-8")
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("move", completed.stderr.lower())

    def test_opening_prefix_without_engine_play_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1/2-1/2"])
            black = write_match(root, "black", ["1/2-1/2"])
            for path in (white, black):
                data = json.loads(path.read_text(encoding="utf-8"))
                data["games"][0]["moves"] = [{"move": "e2e4", "replay_legal": True}]
                path.write_text(json.dumps(data), encoding="utf-8")
                path.with_suffix(".pgn").write_text(
                    '[Event "match"]\n[Result "1/2-1/2"]\n[MoveFormat "UCI coordinate notation"]\n\n1. e2e4 1/2-1/2',
                    encoding="utf-8",
                )
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("engine", completed.stderr.lower())

    def test_pgn_movetext_must_match_json_moves(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1/2-1/2"])
            black = write_match(root, "black", ["1/2-1/2"])
            for path in (white, black):
                data = json.loads(path.read_text(encoding="utf-8"))
                data["games"][0]["moves"] = [
                    {"move": "e2e4", "replay_legal": True},
                    {"move": "e7e5", "replay_legal": True},
                ]
                path.write_text(json.dumps(data), encoding="utf-8")
            white.with_suffix(".pgn").write_text(
                '[Event "match"]\n[Result "1/2-1/2"]\n[MoveFormat "UCI coordinate notation"]\n\n1. e2e4 d7d5 1/2-1/2',
                encoding="utf-8",
            )
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("json and pgn game 1 result or moves differ", completed.stderr.lower())

    def test_forged_replay_legal_flag_does_not_make_illegal_move_valid(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1/2-1/2"])
            black = write_match(root, "black", ["1/2-1/2"])
            data = json.loads(white.read_text(encoding="utf-8"))
            data["games"][0]["moves"][0]["move"] = "e2e5"
            white.write_text(json.dumps(data), encoding="utf-8")
            white.with_suffix(".pgn").write_text(
                '[Event "match"]\n[Result "1/2-1/2"]\n[MoveFormat "UCI coordinate notation"]\n\n1. e2e5 1/2-1/2',
                encoding="utf-8",
            )
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("illegal", completed.stderr.lower())

    def test_pairs_color_reversed_games_and_reports_explicit_cap_draws(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0", "*", "1/2-1/2"])
            black = write_match(root, "black", ["0-1", "*", "1/2-1/2"])
            output = root / "report.json"
            completed = invoke(white, black, output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["paired_openings"], 3)
        self.assertEqual(report["games"], 6)
        self.assertEqual(report["draws"], 4)
        self.assertEqual(report["capped_games_counted_as_draws"], 2)
        self.assertEqual(report["decision"], "inconclusive")
        self.assertEqual(report["threshold_elo"], -10)
        self.assertFalse(report["lower_bound_above_threshold"])
        self.assertIn("Hoeffding", report["method"])

    def test_wrong_candidate_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0"])
            black = write_match(root, "black", ["0-1"])
            completed = invoke(white, black, root / "out.json", candidate_hash="c" * 64)
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("candidate", completed.stderr.lower())

    def test_non_reversed_or_different_opening_pairs_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0", "0-1"])
            black = write_match(root, "black", ["0-1"])
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertRegex(completed.stderr.lower(), "pair|opening|position")

    def test_same_opening_names_with_different_moves_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0"])
            black_data = match("black", ["0-1"])
            black_data["positions"][0]["Moves"] = ["d2d4"]
            black = root / "black.json"
            black.write_text(json.dumps(black_data), encoding="utf-8")
            black.with_suffix(".pgn").write_text('[Result "0-1"]\n\n1. e2e4 0-1', encoding="utf-8")
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("opening", completed.stderr.lower())

    def test_configuration_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0"])
            black_data = match("black", ["0-1"])
            black_data["configuration"]["threads"] = 4
            black = root / "black.json"
            black.write_text(json.dumps(black_data), encoding="utf-8")
            black.with_suffix(".pgn").write_text('[Result "0-1"]\n\n1. e2e4 0-1', encoding="utf-8")
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("config", completed.stderr.lower())

    def test_zero_adverse_pairs_uses_exact_cp_bound_and_can_clear_minus_ten(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1/2-1/2"] * 128)
            black = write_match(root, "black", ["1/2-1/2"] * 128)
            output = root / "out.json"
            completed = invoke(white, black, output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["adverse_opening_pairs"], 0)
        self.assertAlmostEqual(report["adverse_probability_upper_bound"], 1 - 0.05 ** (1 / 128), places=10)
        self.assertAlmostEqual(report["cp_lower_score_bound"], 0.5 * (0.05 ** (1 / 128)), places=10)
        self.assertGreater(report["cp_lower_elo_bound"], -10)
        self.assertEqual(report["decision"], "pass")
        self.assertEqual(report["decision_bound"], "exact_cp_adverse_pair")
        self.assertIn("IID", report["method"])

    def test_candidate_and_baseline_effective_book_settings_must_match(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            white = write_match(root, "white", ["1-0"])
            black = write_match(root, "black", ["0-1"])
            white_data = json.loads(white.read_text(encoding="utf-8"))
            white_data["engines"][0]["handshake"] = [
                "option name OwnBook type check default true",
                "option name RandomSeed type spin default 0 min 0 max 9",
            ]
            white_data["engines"][1]["handshake"] = list(white_data["engines"][0]["handshake"])
            white_data["engines"][0]["options"].append("setoption name OwnBook value false")
            white_data["engines"][1]["options"].append("setoption name OwnBook value true")
            white.write_text(json.dumps(white_data), encoding="utf-8")
            completed = invoke(white, black, root / "out.json")
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("ownbook", completed.stderr.lower())

    def test_positive_adverse_count_uses_exact_cp_bound(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            # Pair 0 is a loss for candidate in both colors; remaining pairs tie.
            white = write_match(root, "white", ["0-1"] + ["1/2-1/2"] * 127)
            black = write_match(root, "black", ["1-0"] + ["1/2-1/2"] * 127)
            output = root / "out.json"
            completed = invoke(white, black, output)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["adverse_opening_pairs"], 1)
        self.assertAlmostEqual(report["adverse_probability_upper_bound"], 0.03652382505354852, places=10)
        self.assertIn("IID", report["method"])
        self.assertEqual(report["decision"], "inconclusive")


if __name__ == "__main__":
    unittest.main()
