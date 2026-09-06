import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools import elo_estimate


def opening_text():
    return "\n".join(f"opening-{index:02d} | e2e4 e7e5" for index in range(1, 33))


def make_paths(directory):
    paths = {}
    for name in ("koi.exe", "replay.exe", "stockfish.exe", "lower.exe", "book.bin"):
        path = directory / name
        path.write_bytes(f"fake {name}".encode("ascii"))
        paths[name] = path
    opening_path = directory / "openings.txt"
    opening_path.write_text(opening_text(), encoding="utf-8")
    paths["openings"] = opening_path
    return paths


def write_anchor_manifest(directory, paths, *, elos=(1400, 1600, 1800), lower=()):
    manifest_path = directory / "anchors.json"
    manifest_path.write_text(json.dumps({
        "schema": "koi-elo-anchor-manifest-v1",
        "stockfish": {"path": str(paths["stockfish.exe"]), "elos": list(elos),
                      "rating_source": "Stockfish 19 UCI_LimitStrength"},
        "lower_anchors": list(lower),
    }), encoding="utf-8")
    return manifest_path


def requested_args(paths, manifest_path, output_path, *extra):
    return [
        "--koi", str(paths["koi.exe"]), "--replay", str(paths["replay.exe"]),
        "--stockfish", str(paths["stockfish.exe"]), "--anchors", str(manifest_path),
        "--openings", str(paths["openings"]), "--time-control", "1+0", "--threads", "4",
        "--hash", "512", "--speed", "100", "--min-games", "128", "--max-games", "128",
        "--prior-elo", "1500", "--mode", "no-book", "--output", str(output_path), *extra,
    ]


def option_line(name, value):
    return f"setoption name {name} value {str(value).lower() if isinstance(value, bool) else value}"


def make_match_report(paths, anchor, color, *, result="1-0", own_book=False):
    koi_options = [
        option_line("Hash", 512), option_line("Threads", 4), option_line("Speed", 100),
        option_line("RandomSeed", 1), option_line("OwnBook", own_book),
        option_line("BookFile", paths["book.bin"] if own_book else "book.bin"),
        option_line("BookDepth", 16), option_line("BookRandom", False),
    ]
    opponent_options = [option_line("Hash", 512), option_line("Threads", 4), option_line("Speed", 100)]
    if anchor.stockfish_elo is not None:
        opponent_options += [option_line("UCI_LimitStrength", True), option_line("UCI_Elo", anchor.rating)]
    return {
        "schema": "koi-uci-match-v2",
        "configuration": {"time_control": "1+0", "movetime_ms": 0, "threads": 4, "speed": 100,
                          "hash_mb": 512, "games_per_position": 1, "koi_color": color,
                          "koi_own_book": own_book,
                          "koi_book_file": str(paths["book.bin"] if own_book else "book.bin"),
                          "koi_book_depth": 16},
        "engines": [
            {"label": "Koi", "path": str(paths["koi.exe"]), "name": "Koi 1.2.3",
             "identity": ["id name Koi 1.2.3"], "options": koi_options,
             "process_status": "clean shutdown", "exit_code": 0},
            {"label": "Opponent", "path": str(anchor.path), "name": "Stockfish 19",
             "identity": ["id name Stockfish 19"], "options": opponent_options,
             "process_status": "clean shutdown", "exit_code": 0},
        ],
        "positions": [{"Name": f"opening-{index:02d}"} for index in range(1, 33)],
        "games": [
            {"position": f"opening-{index:02d}", "koi_color": color, "result": result,
             "termination": "checkmate", "process_status": {"koi": "clean shutdown", "opponent": "clean shutdown"},
             "moves": [{"replay_legal": True}]}
            for index in range(1, 33)
        ],
    }


class EloEstimateTests(unittest.TestCase):
    def test_anchor_manifest_requires_schema_matching_stockfish_and_lower_anchor_when_needed(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest_path = write_anchor_manifest(directory, paths)
            manifest = elo_estimate.load_anchor_manifest(manifest_path, paths["stockfish.exe"], 1500)
            self.assertEqual([anchor.rating for anchor in manifest.anchors], [1400, 1600, 1800])
            other_stockfish = directory / "other-stockfish.exe"
            other_stockfish.write_bytes(b"other")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "must match"):
                elo_estimate.load_anchor_manifest(manifest_path, other_stockfish, 1500)
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "external lower anchor"):
                elo_estimate.load_anchor_manifest(manifest_path, paths["stockfish.exe"], 1000)
            lower = {"id": "baseline-1000", "path": str(paths["lower.exe"]), "rating": 1000,
                     "rating_source": "published baseline"}
            loaded = elo_estimate.load_anchor_manifest(write_anchor_manifest(directory, paths, lower=(lower,)), paths["stockfish.exe"], 1000)
            self.assertEqual([anchor.id for anchor in loaded.anchors][:2], ["baseline-1000", "stockfish-1400"])
            wrong = json.loads(manifest_path.read_text(encoding="utf-8"))
            wrong["schema"] = "wrong"
            manifest_path.write_text(json.dumps(wrong), encoding="utf-8")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "schema"):
                elo_estimate.load_anchor_manifest(manifest_path, paths["stockfish.exe"], 1500)

    def test_schedule_sizes_are_exact_and_initial_batch_brackets_prior(self):
        anchors = tuple(elo_estimate.Anchor(f"stockfish-{rating}", Path(f"C:/{rating}.exe"), rating, "test", rating)
                        for rating in (1400, 1600, 1800, 2000))
        for total_games, batch_count in {128: 2, 192: 3, 256: 4, 320: 5}.items():
            with self.subTest(total_games=total_games):
                schedule = elo_estimate.plan_schedule(anchors, prior_elo=1700, target_games=total_games)
                self.assertEqual(len(schedule), batch_count)
                self.assertEqual(sum(batch.games for batch in schedule), total_games)
                self.assertEqual([batch.anchor.rating for batch in schedule[:2]], [1600, 1800])
                self.assertTrue(all(batch.games == 64 for batch in schedule))

    def test_result_normalization_and_report_validation_rejects_bad_game_evidence(self):
        self.assertEqual(elo_estimate.normalize_result("1-0", "white"), 1.0)
        self.assertEqual(elo_estimate.normalize_result("1-0", "black"), 0.0)
        self.assertEqual(elo_estimate.normalize_result("1/2-1/2", "black"), 0.5)
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest = elo_estimate.load_anchor_manifest(write_anchor_manifest(directory, paths), paths["stockfish.exe"], 1500)
            anchor, options = manifest.anchors[1], elo_estimate.koi_option_set("no-book", None)
            report = make_match_report(paths, anchor, "white")
            games = elo_estimate.validate_match_manifest(report, tuple(f"opening-{index:02d}" for index in range(1, 33)), "white", anchor, options, "1+0", None, paths["koi.exe"])
            self.assertEqual(len(games), 32)
            self.assertTrue(all(game.score == 1.0 for game in games))
            for mutation, expected in (
                (lambda value: value["games"][0].update({"result": "*"}), "incomplete"),
                (lambda value: value["games"][0]["moves"][0].update({"replay_legal": False}), "illegal"),
                (lambda value: value["games"][0]["process_status"].update({"koi": "timeout"}), "process"),
                (lambda value: value["engines"][1]["options"].pop(), "option"),
                (lambda value: value["games"][0].update({"termination": "max plies"}), "incomplete"),
                (lambda value: value["games"][0].update({"termination": "process exit"}), "incomplete"),
                (lambda value: value["games"][0].pop("moves"), "malformed"),
                (lambda value: value.pop("positions"), "positions"),
                (lambda value: value["games"][0].update({"termination": "unknown terminal"}), "termination"),
            ):
                broken = json.loads(json.dumps(report))
                mutation(broken)
                with self.subTest(expected=expected), self.assertRaisesRegex(elo_estimate.EloEstimateError, expected):
                    elo_estimate.validate_match_manifest(broken, tuple(f"opening-{index:02d}" for index in range(1, 33)), "white", anchor, options, "1+0", None, paths["koi.exe"])
            adjudicated = make_match_report(paths, anchor, "white", result="*")
            for game in adjudicated["games"]:
                game["termination"] = "adjudicated draw"
            self.assertEqual(elo_estimate.validate_match_manifest(adjudicated, tuple(f"opening-{index:02d}" for index in range(1, 33)), "white", anchor, options, "1+0", None, paths["koi.exe"])[0].score, 0.5)

    def test_logistic_helpers_have_known_50_75_and_25_percent_values(self):
        self.assertEqual(elo_estimate.expected_score(1800, 1800), 0.5)
        self.assertAlmostEqual(elo_estimate.rating_delta_for_score(0.75), 190.8485, places=3)
        self.assertAlmostEqual(elo_estimate.rating_delta_for_score(0.25), -190.8485, places=3)

    def test_fit_uses_jeffreys_smoothing_and_reports_bounds_outside_bracket(self):
        fit = elo_estimate.fit_rating({1400: (0.0,) * 64, 1600: (0.0,) * 64})
        self.assertIsNone(fit.elo)
        self.assertEqual((fit.lower_bound, fit.upper_bound), (1400, 1600))
        self.assertEqual(fit.smoothed_scores[1400], 0.5 / 65)
        self.assertEqual(elo_estimate.fit_rating({1400: (1.0,) * 32, 1600: (0.0,) * 32}).elo, 1500)

    def test_paired_bootstrap_is_deterministic_uses_2000_samples_and_has_ci(self):
        pairs = {1400: [(1.0, 1.0)] * 32, 1600: [(0.0, 0.0)] * 32}
        first, second = elo_estimate.paired_bootstrap(pairs, seed=17), elo_estimate.paired_bootstrap(pairs, seed=17)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 2000)
        self.assertEqual(elo_estimate.bootstrap_ci(first), (1500, 1500))

    def test_no_book_and_book_options_are_distinct_and_book_random_is_false(self):
        self.assertEqual(elo_estimate.koi_option_set("no-book", None), {"Hash": 512, "Threads": 4, "Speed": 100, "OwnBook": False})
        with_book = elo_estimate.koi_option_set("book", Path("C:/licensed/book.bin"))
        self.assertEqual((with_book["OwnBook"], with_book["BookRandom"], with_book["BookDepth"]), (True, False, 16))

    def test_reproducibility_hash_excludes_timestamp_fields(self):
        before = {
            "timestamp": "old", "configuration": {"hashes": {"koi": "stable-executable-hash"}},
            "artifacts": [{"kind": "json", "path": "C:/results/koi-uci-match-20260906-100000.json", "sha256": "old-artifact-hash", "content_metadata": {"generated_utc": "old"}}],
        }
        after = {
            "timestamp": "new", "configuration": {"hashes": {"koi": "stable-executable-hash"}},
            "artifacts": [{"kind": "json", "path": "C:/results/koi-uci-match-20260906-110000.json", "sha256": "new-artifact-hash", "content_metadata": {"generated_utc": "new"}}],
        }
        self.assertEqual(elo_estimate.reproducibility_hash(before), elo_estimate.reproducibility_hash(after))
        after["configuration"]["hashes"]["koi"] = "changed-executable-hash"
        self.assertNotEqual(elo_estimate.reproducibility_hash(before), elo_estimate.reproducibility_hash(after))

    def test_dry_run_validates_and_writes_full_schedule_without_launching(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            output_path = directory / "external-report.json"
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--max-games", "320", "--dry-run")
            with mock.patch.object(elo_estimate.subprocess, "run") as run:
                self.assertEqual(elo_estimate.main(arguments), 0)
            run.assert_not_called()
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "koi-rough-elo-estimate-v1")
            self.assertTrue({"estimate", "results", "anchors", "batches", "configuration", "artifacts"}.issubset(report))
            self.assertEqual(len(report["batches"]), 5)
            self.assertEqual(report["results"], {"games": 0, "wins": 0, "draws": 0, "losses": 0, "score": 0.0})
            self.assertIn("manifest", report["configuration"]["hashes"])

    def test_mocked_non_dry_run_invokes_powershell_harness_and_preserves_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            output_path = directory / "external-report.json"
            calls = []
            def fake_run(command, **_kwargs):
                calls.append(command)
                output_directory = Path(command[command.index("-OutputDirectory") + 1])
                color = command[command.index("-KoiColor") + 1]
                rating = int(command[command.index("-OpponentElo") + 1])
                anchor = elo_estimate.Anchor(f"stockfish-{rating}", paths["stockfish.exe"], rating, "test", rating)
                output_directory.mkdir(parents=True, exist_ok=True)
                report_path, pgn_path = output_directory / f"{rating}-{color}.json", output_directory / f"{rating}-{color}.pgn"
                result = "1-0" if color == "white" else "0-1"
                report_path.write_text(json.dumps(make_match_report(paths, anchor, color, result=result)), encoding="utf-8")
                pgn_path.write_text("[Result \"1-0\"]\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, f"json {report_path}\npgn {pgn_path}\n", "")
            with mock.patch.object(elo_estimate, "_powershell_executable", return_value="powershell-test"), mock.patch.object(elo_estimate.subprocess, "run", side_effect=fake_run):
                self.assertEqual(elo_estimate.main(requested_args(paths, write_anchor_manifest(directory, paths), output_path)), 0)
            self.assertEqual(len(calls), 4)
            self.assertTrue(all(command[command.index("-File") + 1].endswith("uci_match.ps1") for command in calls))
            self.assertTrue(all(command[command.index("-Hash") + 1] == "512" for command in calls))
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["results"]["games"], 128)
            self.assertEqual(len(report["artifacts"]), 8)
            self.assertIn("reproducibility_hash", report)

    def test_low_confidence_non_dry_run_adds_an_adaptive_64_game_batch_up_to_maximum(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            output_path = directory / "external-report.json"
            calls = []
            def fake_run(command, **_kwargs):
                calls.append(command)
                output_directory = Path(command[command.index("-OutputDirectory") + 1])
                color = command[command.index("-KoiColor") + 1]
                rating = int(command[command.index("-OpponentElo") + 1])
                anchor = elo_estimate.Anchor(f"stockfish-{rating}", paths["stockfish.exe"], rating, "test", rating)
                output_directory.mkdir(parents=True, exist_ok=True)
                report_path, pgn_path = output_directory / f"{rating}-{color}.json", output_directory / f"{rating}-{color}.pgn"
                result = "1-0" if color == "white" else "0-1"
                report_path.write_text(json.dumps(make_match_report(paths, anchor, color, result=result)), encoding="utf-8")
                pgn_path.write_text("[Result \"1-0\"]\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, f"json {report_path}\npgn {pgn_path}\n", "")
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--max-games", "192")
            with mock.patch.object(elo_estimate, "_powershell_executable", return_value="powershell-test"), mock.patch.object(elo_estimate.subprocess, "run", side_effect=fake_run):
                self.assertEqual(elo_estimate.main(arguments), 0)
            self.assertEqual(len(calls), 6)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["results"]["games"], 192)
            self.assertEqual(report["batches"][-1]["phase"], "adaptive")


if __name__ == "__main__":
    unittest.main()
