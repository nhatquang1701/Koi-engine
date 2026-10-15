import contextlib
import hashlib
import io
import json
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.measurement import elo_estimate


def opening_text():
    return "\n".join(f"opening-{index:02d} | e2e4 e7e5" for index in range(1, 33))


def opening_map():
    return {f"opening-{index:02d}": ("e2e4", "e7e5") for index in range(1, 33)}


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


def make_eval_file(directory, version=4, name="net.nnue"):
    path = directory / name
    path.write_bytes(b"KOI-NNUE" + version.to_bytes(4, "little") + b"\x00" * 8)
    return path


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


def make_match_report(paths, anchor, color, *, result="1-0", own_book=False, eval_file=None):
    openings = opening_map()
    koi_options = [
        option_line("Hash", 512), option_line("Threads", 4), option_line("Speed", 100),
        option_line("RandomSeed", 1), option_line("OwnBook", own_book),
        option_line("BookFile", paths["book.bin"] if own_book else "book.bin"),
        option_line("BookDepth", 16), option_line("BookRandom", False),
    ]
    opponent_options = [option_line("Hash", 512), option_line("Threads", 4), option_line("Speed", 100)]
    if anchor.stockfish_elo is not None:
        opponent_options += [option_line("UCI_LimitStrength", True), option_line("UCI_Elo", anchor.rating)]
    configuration = {"time_control": "1+0", "movetime_ms": 0, "threads": 4, "speed": 100,
                     "hash_mb": 512, "games_per_position": 1, "koi_color": color,
                     "koi_random_seed": 1,
                     "koi_own_book": own_book,
                     "koi_book_file": str(paths["book.bin"] if own_book else "book.bin"),
                     "koi_book_depth": 16, "koi_book_random": False,
                     "opponent_limit_strength": anchor.stockfish_elo is not None,
                     "opponent_elo": anchor.rating if anchor.stockfish_elo is not None else None}
    if eval_file is not None:
        data = eval_file.read_bytes()
        configuration.update({
            "koi_evaluator_mode": {4: "nnue-v4", 5: "nnue-v5"}[int.from_bytes(data[8:12], "little")],
            "koi_evalfile_path": str(eval_file),
            "koi_evalfile_sha256": hashlib.sha256(data).hexdigest(),
            "koi_evaluator_attestation": {
                "nnue_enabled_confirmation": f"info string NNUE enabled from {eval_file}",
                "evalfile_rejections": [],
                "gpu_enabled_marker": None,
                "gpu_unavailable_fallbacks": [],
            },
        })
        koi_options.append(option_line("EvalFile", str(eval_file)))
    return {
        "schema": "koi-uci-match-v2",
        "configuration": configuration,
        "engines": [
            {"label": "Koi", "path": str(paths["koi.exe"]), "name": "Koi 1.2.3",
             "identity": ["id name Koi 1.2.3"], "options": koi_options,
             "process_status": "clean shutdown", "exit_code": 0},
            {"label": "Opponent", "path": str(anchor.path), "name": "Stockfish 19",
             "identity": ["id name Stockfish 19"], "options": opponent_options,
             "process_status": "clean shutdown", "exit_code": 0},
        ],
        "positions": [{"Name": name, "Fen": "startpos", "Moves": list(moves)}
                      for name, moves in openings.items()],
        "games": [
            {"position": name, "initial_fen": "startpos", "koi_color": color, "result": result,
             "termination": "checkmate", "process_status": {"koi": "clean shutdown", "opponent": "clean shutdown"},
             "moves": ([{"move": move, "engine_label": "opening", "replay_legal": True}
                        for move in moves] + [{"move": "g1f3", "engine_label": "Koi", "replay_legal": True}])}
            for name, moves in openings.items()
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

    def test_calibration_sweep_includes_each_anchor_before_adaptive_sampling(self):
        anchors = tuple(elo_estimate.Anchor(f"stockfish-{rating}", Path(f"C:/{rating}.exe"), rating, "test", rating)
                        for rating in (1400, 1600, 1800))
        schedule = elo_estimate.plan_schedule(
            anchors, prior_elo=1600, target_games=192, include_all_anchors=True
        )
        self.assertEqual([batch.anchor.rating for batch in schedule], [1400, 1600, 1800])
        self.assertTrue(all(batch.phase == "initial" for batch in schedule))
        self.assertEqual(sum(batch.games for batch in schedule), 192)

    def test_calibration_sweep_requires_one_initial_batch_for_each_anchor(self):
        anchors = tuple(elo_estimate.Anchor(f"stockfish-{rating}", Path(f"C:/{rating}.exe"), rating, "test", rating)
                        for rating in (1400, 1600, 1800))
        with self.assertRaisesRegex(elo_estimate.EloEstimateError, "one initial batch per anchor"):
            elo_estimate.plan_schedule(
                anchors, prior_elo=1600, target_games=128, include_all_anchors=True
            )

    def test_all_anchor_schedule_exports_and_imports_every_initial_anchor(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest_path = write_anchor_manifest(directory, paths)
            manifest = elo_estimate.load_anchor_manifest(manifest_path, paths["stockfish.exe"], 1600)
            schedule = elo_estimate.plan_schedule(
                manifest.anchors,
                prior_elo=1600,
                target_games=192,
                opening_names=tuple(opening_map()),
                include_all_anchors=True,
            )
            schedule_path = directory / "all-anchors-schedule.json"
            payload = elo_estimate.export_schedule(
                schedule_path,
                schedule,
                prior_elo=1600,
                opening_names=tuple(opening_map()),
                include_all_anchors=True,
            )
            restored = elo_estimate.import_schedule(
                schedule_path,
                manifest.anchors,
                expected_openings=tuple(opening_map()),
                prior_elo=1600,
                require_all_anchors=True,
            )
            self.assertEqual(payload["schema"], "koi-elo-schedule-v2")
            self.assertEqual([batch.anchor.rating for batch in restored], [1400, 1600, 1800])

    def test_all_anchor_dry_run_records_sweep_and_schedule_provenance(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest_path = write_anchor_manifest(directory, paths)
            output_path = directory / "dry-run.json"
            schedule_path = directory / "schedule.json"
            arguments = requested_args(
                paths,
                manifest_path,
                output_path,
                "--min-games", "192",
                "--max-games", "192",
                "--all-anchors",
                "--schedule-export", str(schedule_path),
                "--dry-run",
            )
            self.assertEqual(elo_estimate.main(arguments), 0)
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual([batch["anchor_rating"] for batch in report["batches"]], [1400, 1600, 1800])
            self.assertTrue(report["configuration"]["measurement"]["all_anchors"])
            self.assertEqual(report["schedule"]["schema"], "koi-elo-schedule-v2")

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
            games = elo_estimate.validate_match_manifest(report, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"])
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
                    elo_estimate.validate_match_manifest(broken, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"])
            adjudicated = make_match_report(paths, anchor, "white", result="*")
            for game in adjudicated["games"]:
                game["termination"] = "adjudicated draw"
            self.assertEqual(elo_estimate.validate_match_manifest(adjudicated, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"])[0].score, 0.5)

    def test_report_validation_rejects_conflicting_options_and_configuration_drift(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest = elo_estimate.load_anchor_manifest(write_anchor_manifest(directory, paths), paths["stockfish.exe"], 1500)
            anchor, options = manifest.anchors[1], elo_estimate.koi_option_set("no-book", None)
            report = make_match_report(paths, anchor, "white")
            for mutation, expected in (
                (lambda value: value["engines"][0]["options"].append(option_line("OwnBook", True)), "conflicting"),
                (lambda value: value["configuration"].update({"koi_own_book": True}), "configuration"),
                (lambda value: value["configuration"].update({"opponent_elo": anchor.rating + 1}), "configuration"),
                (lambda value: value["configuration"].update({"opponent_limit_strength": False}), "configuration"),
                (lambda value: value["engines"][0]["options"].append(option_line("UCI_LimitStrength", True)), "strength"),
            ):
                broken = json.loads(json.dumps(report))
                mutation(broken)
                with self.subTest(expected=expected), self.assertRaisesRegex(elo_estimate.EloEstimateError, expected):
                    elo_estimate.validate_match_manifest(broken, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"])

            book_options = elo_estimate.koi_option_set("book", paths["book.bin"])
            book_report = make_match_report(paths, anchor, "white", own_book=True)
            book_report["configuration"]["koi_book_file"] = str(directory / "other-book.bin")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "configuration"):
                elo_estimate.validate_match_manifest(book_report, opening_map(), "white", anchor, book_options, "1+0", None, paths["koi.exe"])

    def test_report_validation_binds_opening_positions_to_the_requested_sequences(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            manifest = elo_estimate.load_anchor_manifest(write_anchor_manifest(directory, paths), paths["stockfish.exe"], 1500)
            anchor, options = manifest.anchors[1], elo_estimate.koi_option_set("no-book", None)
            report = make_match_report(paths, anchor, "white")
            for mutation in (
                lambda value: value["positions"][0].update({"Fen": "8/8/8/8/8/8/8/8 w - - 0 1"}),
                lambda value: value["positions"][0].update({"Moves": ["d2d4", "d7d5"]}),
                lambda value: value["games"][0]["moves"][0].update({"move": "d2d4"}),
            ):
                broken = json.loads(json.dumps(report))
                mutation(broken)
                with self.subTest(), self.assertRaisesRegex(elo_estimate.EloEstimateError, "opening"):
                    elo_estimate.validate_match_manifest(broken, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"])

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
            self.assertEqual(report["configuration"]["measurement"], {
                "label": "Local Stockfish-equivalent Elo at the recorded hardware, engine versions, options, and time control.",
                "perspective": "Koi",
                "prior_elo": 1500,
                "min_games": 128,
                "max_games": 320,
                "all_anchors": False,
            })
            for field, changed_value in (("prior_elo", 1501), ("min_games", 192), ("max_games", 192)):
                changed_schedule = json.loads(json.dumps(report))
                changed_schedule["configuration"]["measurement"][field] = changed_value
                with self.subTest(field=field):
                    self.assertNotEqual(elo_estimate.reproducibility_hash(report), elo_estimate.reproducibility_hash(changed_schedule))

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

    def test_koi_eval_file_is_plumbed_to_the_match_command_and_option_allowlist(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            eval_file = make_eval_file(directory).resolve()
            evaluator = elo_estimate.load_koi_evaluator(eval_file)
            self.assertEqual((evaluator.mode, evaluator.container_version), ("nnue-v4", 4))
            self.assertEqual(evaluator.size_bytes, eval_file.stat().st_size)
            self.assertEqual(evaluator.sha256, hashlib.sha256(eval_file.read_bytes()).hexdigest())
            anchor = elo_estimate.Anchor("stockfish-1600", paths["stockfish.exe"], 1600, "test", 1600)
            classical_options = elo_estimate.koi_option_set("no-book", None)
            command = elo_estimate._harness_command(
                "powershell-test", paths["koi.exe"], paths["replay.exe"], anchor, paths["openings"],
                directory / "out", "white", "1+0", None, classical_options, evaluator=evaluator,
            )
            self.assertEqual(command[command.index("-KoiEvalFile") + 1], str(eval_file))
            self.assertEqual(command[command.index("-KoiEvaluatorMode") + 1], "nnue-v4")
            classical = elo_estimate._harness_command(
                "powershell-test", paths["koi.exe"], paths["replay.exe"], anchor, paths["openings"],
                directory / "out", "white", "1+0", None, classical_options,
            )
            self.assertNotIn("-KoiEvalFile", classical)
            self.assertNotIn("-KoiEvaluatorMode", classical)
            parsed = elo_estimate._parse_relevant_options({"options": [option_line("EvalFile", str(eval_file))]}, "Koi")
            self.assertEqual(list(parsed), ["EvalFile"])
            self.assertTrue(elo_estimate._same_path(Path(parsed["EvalFile"]), eval_file))

    def test_koi_eval_file_provenance_and_attestation_are_recorded(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            eval_file = make_eval_file(directory).resolve()
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
                attested = Path(command[command.index("-KoiEvalFile") + 1])
                report_path.write_text(json.dumps(make_match_report(paths, anchor, color, result=result, eval_file=attested)), encoding="utf-8")
                pgn_path.write_text("[Result \"1-0\"]\n", encoding="utf-8")
                return subprocess.CompletedProcess(command, 0, f"json {report_path}\npgn {pgn_path}\n", "")
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--koi-eval-file", str(eval_file))
            with mock.patch.object(elo_estimate, "_powershell_executable", return_value="powershell-test"), mock.patch.object(elo_estimate.subprocess, "run", side_effect=fake_run):
                self.assertEqual(elo_estimate.main(arguments), 0)
            self.assertEqual(len(calls), 4)
            self.assertTrue(all(command[command.index("-KoiEvalFile") + 1] == str(eval_file) for command in calls))
            self.assertTrue(all(command[command.index("-KoiEvaluatorMode") + 1] == "nnue-v4" for command in calls))
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["results"]["games"], 128)
            evaluator = report["configuration"]["koi_evaluator"]
            self.assertTrue(elo_estimate._same_path(Path(evaluator["eval_file"]["path"]), eval_file))
            self.assertEqual(evaluator["eval_file"]["sha256"], hashlib.sha256(eval_file.read_bytes()).hexdigest())
            self.assertEqual(evaluator["eval_file"]["size_bytes"], eval_file.stat().st_size)
            self.assertEqual(evaluator["eval_file"]["container_version"], 4)
            self.assertEqual(evaluator["eval_file"]["evaluator_mode"], "nnue-v4")
            self.assertEqual(report["configuration"]["active_koi_options"]["EvalFile"], str(eval_file))
            self.assertEqual(evaluator["attestation"]["nnue_enabled_confirmation"], f"info string NNUE enabled from {eval_file}")
            self.assertEqual(evaluator["attestation"]["evalfile_rejections"], [])
            self.assertNotIn("koi_options_json", report["configuration"])

    def test_koi_eval_file_dry_run_records_provenance_without_attestation(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            eval_file = make_eval_file(directory).resolve()
            output_path = directory / "external-report.json"
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--koi-eval-file", str(eval_file), "--dry-run")
            with mock.patch.object(elo_estimate.subprocess, "run") as run:
                self.assertEqual(elo_estimate.main(arguments), 0)
            run.assert_not_called()
            report = json.loads(output_path.read_text(encoding="utf-8"))
            evaluator = report["configuration"]["koi_evaluator"]
            self.assertTrue(elo_estimate._same_path(Path(evaluator["eval_file"]["path"]), eval_file))
            self.assertEqual(evaluator["eval_file"]["sha256"], hashlib.sha256(eval_file.read_bytes()).hexdigest())
            self.assertEqual(evaluator["eval_file"]["size_bytes"], eval_file.stat().st_size)
            self.assertIsNone(evaluator["attestation"])

    def test_missing_or_invalid_koi_eval_file_fails_loudly(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            missing = directory / "absent.nnue"
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "missing"):
                elo_estimate.load_koi_evaluator(missing)
            invalid = directory / "invalid.nnue"
            invalid.write_bytes(b"not a koi nnue container")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "container"):
                elo_estimate.load_koi_evaluator(invalid)
            unsupported = make_eval_file(directory, version=6, name="v6.nnue")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "version 6"):
                elo_estimate.load_koi_evaluator(unsupported)
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), directory / "external-report.json", "--koi-eval-file", str(missing))
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(elo_estimate.main(arguments), 2)
            self.assertIn("Koi EvalFile is missing", stderr.getvalue())

    def test_report_validation_requires_a_matching_eval_file_attestation(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            eval_file = make_eval_file(directory).resolve()
            evaluator = elo_estimate.load_koi_evaluator(eval_file)
            manifest = elo_estimate.load_anchor_manifest(write_anchor_manifest(directory, paths), paths["stockfish.exe"], 1500)
            anchor = manifest.anchors[1]
            options = elo_estimate.koi_option_set("no-book", None)
            options["EvalFile"] = str(eval_file)
            report = make_match_report(paths, anchor, "white", eval_file=eval_file)
            games = elo_estimate.validate_match_manifest(report, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"], evaluator)
            self.assertEqual(len(games), 32)
            for mutation, expected in (
                (lambda value: value["configuration"].update({"koi_evaluator_mode": "classical"}), "mode"),
                (lambda value: value["configuration"].update({"koi_evalfile_path": str(directory / "other.nnue")}), "path"),
                (lambda value: value["configuration"].update({"koi_evalfile_sha256": "0" * 64}), "SHA-256"),
                (lambda value: value["configuration"].pop("koi_evaluator_attestation"), "attestation"),
                (lambda value: value["configuration"]["koi_evaluator_attestation"].update({"evalfile_rejections": ["info string EvalFile rejected"]}), "rejected"),
                (lambda value: value["configuration"]["koi_evaluator_attestation"].update({"nnue_enabled_confirmation": None}), "confirmation"),
                (lambda value: value["engines"][0]["options"].pop(), "option"),
            ):
                broken = json.loads(json.dumps(report))
                mutation(broken)
                with self.subTest(expected=expected), self.assertRaisesRegex(elo_estimate.EloEstimateError, expected):
                    elo_estimate.validate_match_manifest(broken, opening_map(), "white", anchor, options, "1+0", None, paths["koi.exe"], evaluator)

    def test_koi_options_json_is_forwarded_with_provenance_and_rejects_pinned_options(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            options_path = directory / "koi-options.json"
            options_path.write_text(json.dumps({"GpuNnue": "off"}), encoding="utf-8")
            loaded = elo_estimate.load_koi_options_json(options_path)
            self.assertTrue(elo_estimate._same_path(loaded.path, options_path))
            self.assertEqual(loaded.sha256, hashlib.sha256(options_path.read_bytes()).hexdigest())
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
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--koi-options-json", str(options_path))
            with mock.patch.object(elo_estimate, "_powershell_executable", return_value="powershell-test"), mock.patch.object(elo_estimate.subprocess, "run", side_effect=fake_run):
                self.assertEqual(elo_estimate.main(arguments), 0)
            self.assertTrue(all(command[command.index("-KoiOptionsJsonPath") + 1] == str(loaded.path) for command in calls))
            self.assertTrue(all("-KoiEvalFile" not in command for command in calls))
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(report["configuration"]["koi_options_json"], {"path": str(loaded.path), "sha256": loaded.sha256})
            self.assertNotIn("koi_evaluator", report["configuration"])
            for payload, expected in (
                ({"Threads": 8}, "Threads"),
                ({"EvalFile": "net.nnue"}, "EvalFile"),
                ([1, 2], "object"),
            ):
                broken_path = directory / f"broken-{expected}.json"
                broken_path.write_text(json.dumps(payload), encoding="utf-8")
                with self.subTest(expected=expected), self.assertRaisesRegex(elo_estimate.EloEstimateError, expected):
                    elo_estimate.load_koi_options_json(broken_path)

    def test_classical_runs_do_not_gain_evaluator_or_options_json_provenance(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = make_paths(directory)
            output_path = directory / "external-report.json"
            arguments = requested_args(paths, write_anchor_manifest(directory, paths), output_path, "--dry-run")
            with mock.patch.object(elo_estimate.subprocess, "run") as run:
                self.assertEqual(elo_estimate.main(arguments), 0)
            run.assert_not_called()
            report = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertNotIn("koi_evaluator", report["configuration"])
            self.assertNotIn("koi_options_json", report["configuration"])
            self.assertEqual(report["configuration"]["active_koi_options"], {"Hash": 512, "Threads": 4, "Speed": 100, "OwnBook": False})

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

    def test_schedule_cli_options_are_available_for_reproducible_before_after_runs(self):
        parser = elo_estimate._build_parser()
        arguments = parser.parse_args([
            "--koi", "C:/Koi/koi.exe", "--replay", "C:/Koi/replay.exe",
            "--stockfish", "C:/Engines/stockfish.exe", "--anchors", "C:/inputs/anchors.json",
            "--openings", "C:/inputs/openings.txt", "--prior-elo", "1500",
            "--output", "C:/results/report.json", "--schedule-export", "C:/results/schedule.json",
            "--schedule-import", "C:/inputs/schedule.json", "--run-label", "after", "--all-anchors", "--dry-run",
        ])
        self.assertEqual(arguments.run_label, "after")
        self.assertTrue(arguments.all_anchors)
        self.assertEqual(arguments.schedule_export, Path("C:/results/schedule.json"))
        self.assertEqual(arguments.schedule_import, Path("C:/inputs/schedule.json"))

    def test_schedule_import_rejects_inconsistent_target_and_missing_opening_contract(self):
        anchors = tuple(
            elo_estimate.Anchor(
                f"stockfish-{rating}", Path(f"C:/{rating}.exe"), rating, "test", rating
            )
            for rating in (1400, 1600, 1800)
        )
        openings = tuple(f"opening-{index:02d}" for index in range(1, 33))
        schedule = elo_estimate.plan_schedule(
            anchors, prior_elo=1500, target_games=128, opening_names=openings
        )
        with self.assertRaisesRegex(elo_estimate.EloEstimateError, "opening"):
            elo_estimate.build_schedule_manifest(
                elo_estimate.plan_schedule(anchors, prior_elo=1500, target_games=128),
                prior_elo=1500,
            )

        with tempfile.TemporaryDirectory(prefix="koi invalid schedule ") as temporary_directory:
            schedule_path = Path(temporary_directory) / "schedule.json"
            elo_estimate.export_schedule(
                schedule_path,
                schedule,
                prior_elo=1500,
                opening_names=openings,
            )
            payload = json.loads(schedule_path.read_text(encoding="utf-8"))
            payload["measurement"]["target_games"] = 320
            payload["schedule_sha256"] = elo_estimate.reproducibility_hash(
                {key: value for key, value in payload.items() if key != "schedule_sha256"}
            )
            schedule_path.write_text(json.dumps(payload), encoding="utf-8")

            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "target_games"):
                elo_estimate.import_schedule(
                    schedule_path,
                    anchors,
                    expected_openings=openings,
                    prior_elo=1500,
                )

            payload["measurement"]["target_games"] = 128
            payload["batches"][0]["anchor"]["path"] = "C:/different-stockfish.exe"
            payload["schedule_sha256"] = elo_estimate.reproducibility_hash(
                {key: value for key, value in payload.items() if key != "schedule_sha256"}
            )
            schedule_path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(elo_estimate.EloEstimateError, "anchor"):
                elo_estimate.import_schedule(
                    schedule_path,
                    anchors,
                    expected_openings=openings,
                    prior_elo=1500,
                )


if __name__ == "__main__":
    unittest.main()
