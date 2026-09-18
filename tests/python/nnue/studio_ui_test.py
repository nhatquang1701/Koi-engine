"""Headless tests for the NNUE Studio UI improvement pass.

These cover the pure logic behind the GUI fixes and telemetry: network file
naming, validation gating, numeric input guards, serialized run-state writes,
and the backend command line.  The widget layer itself is only smoke-tested by
``--gui-selftest`` in ``studio_test.py``.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_NNUE = REPO_ROOT / "tools" / "nnue"
STUDIO = TOOLS_NNUE / "koi_nnue_studio.py"


def load_studio_core():
    spec = importlib.util.spec_from_file_location("koi_studio_ui_core", TOOLS_NNUE / "studio_core.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class NetworkNamingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_default_and_sanitized_names(self):
        self.assertEqual(self.core.network_file_name({}), "net.nnue")
        self.assertEqual(self.core.network_file_name({"net_name": ""}), "net.nnue")
        self.assertEqual(self.core.network_file_name({"net_name": "custom.nnue"}), "custom.nnue")
        # A path is reduced to its final component so runs cannot escape the directory.
        self.assertEqual(self.core.network_file_name({"net_name": "a/b/c.nnue"}), "c.nnue")

    def test_configured_name_wins_when_the_file_exists(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "custom.nnue").write_bytes(b"x")
            (root / "net.nnue").write_bytes(b"x")
            resolved = self.core.network_path(root, {"net_name": "custom.nnue"})
            self.assertEqual(resolved.name, "custom.nnue")
            self.assertEqual(self.core.metadata_path(root, {"net_name": "custom.nnue"}).name,
                             "custom.metadata.json")

    def test_legacy_net_nnue_is_the_fallback_for_old_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "net.nnue").write_bytes(b"x")
            resolved = self.core.network_path(root, {"net_name": "koi.nnue"})
            self.assertEqual(resolved.name, "net.nnue")
            self.assertEqual(self.core.metadata_path(root, {"net_name": "koi.nnue"}).name,
                             "net.metadata.json")

    def test_unwritten_run_resolves_to_the_configured_target(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            resolved = self.core.network_path(root, {"net_name": "koi.nnue"})
            self.assertEqual(resolved, root / "koi.nnue")

    def test_run_properties_use_their_own_config(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self.core.Run(directory=Path(directory), config={"net_name": "koi.nnue"})
            self.assertEqual(run.net_path, Path(directory) / "koi.nnue")
            self.assertEqual(run.metadata_path, Path(directory) / "koi.metadata.json")


class ValidationGatingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_gate_allows_install_only_after_a_successful_run(self):
        self.assertTrue(self.core.gate_allows_install({"positions": 64, "matches": 64}))
        self.assertFalse(self.core.gate_allows_install({"positions": 0}))
        self.assertFalse(self.core.gate_allows_install({"positions": 64, "rejected": True}))
        self.assertFalse(self.core.gate_allows_install({"error": "koi-bench not found"}))
        self.assertFalse(self.core.gate_allows_install({}))
        self.assertFalse(self.core.gate_allows_install(None))

    def test_gate_only_skips_the_ab_match(self):
        calls = {"gate": 0, "ab": 0}
        core = self.core
        original_gate, original_ab = core.run_gate, core.run_ab_match

        def fake_gate(net):
            calls["gate"] += 1
            return {"kind": "gate", "positions": 64, "matches": 64}

        def fake_ab(*args, **kwargs):
            calls["ab"] += 1
            return {"score": 10.0}

        core.run_gate, core.run_ab_match = fake_gate, fake_ab
        try:
            result = core.validate_net(Path("unused.nnue"), gate_only=True)
        finally:
            core.run_gate, core.run_ab_match = original_gate, original_ab
        self.assertEqual(calls, {"gate": 1, "ab": 0})
        self.assertIn("gate", result)
        self.assertNotIn("ab_match", result)

    def test_full_validation_runs_both_checks(self):
        calls = {"gate": 0, "ab": 0}
        core = self.core
        original_gate, original_ab = core.run_gate, core.run_ab_match

        def fake_gate(net):
            calls["gate"] += 1
            return {"kind": "gate", "positions": 64, "matches": 64}

        def fake_ab(*args, **kwargs):
            calls["ab"] += 1
            return {"score": 10.0}

        core.run_gate, core.run_ab_match = fake_gate, fake_ab
        try:
            result = core.validate_net(Path("unused.nnue"))
        finally:
            core.run_gate, core.run_ab_match = original_gate, original_ab
        self.assertEqual(calls, {"gate": 1, "ab": 1})
        self.assertIn("ab_match", result)


class InputGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_parse_int_accepts_and_bounds(self):
        self.assertEqual(self.core.parse_int(" 12 ", "Epochs"), 12)
        self.assertEqual(self.core.parse_int("2", "A/B games", 2, 2000), 2)
        with self.assertRaises(ValueError) as context:
            self.core.parse_int("one", "Epochs")
        self.assertIn("Epochs", str(context.exception))
        with self.assertRaises(ValueError):
            self.core.parse_int("1", "A/B games", 2, 2000)
        with self.assertRaises(ValueError):
            self.core.parse_int("2001", "A/B games", 2, 2000)

    def test_parse_float_accepts_and_bounds(self):
        self.assertAlmostEqual(self.core.parse_float("0.05", "Val fraction", 0.0, 1.0), 0.05)
        with self.assertRaises(ValueError) as context:
            self.core.parse_float("half", "Val fraction")
        self.assertIn("Val fraction", str(context.exception))
        with self.assertRaises(ValueError):
            self.core.parse_float("1.5", "Val fraction", 0.0, 1.0)


class StateLockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_concurrent_state_writes_stay_parseable(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self.core.Run(directory=Path(directory))
            threads = [
                threading.Thread(target=run.write_state, kwargs={f"key_{index}": index})
                for index in range(8)
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            payload = json.loads((Path(directory) / "state.json").read_text(encoding="utf-8"))
            self.assertEqual(payload["schema"], self.core.RUN_SCHEMA)
            for index in range(8):
                self.assertEqual(payload[f"key_{index}"], index)


class BackendCommandTests(unittest.TestCase):
    def test_dry_run_honors_the_configured_network_name(self):
        result = subprocess.run(
            [
                sys.executable,
                str(STUDIO),
                "--dry-run",
                "--preset",
                "quick",
                "--net-name",
                "custom.nnue",
            ],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("custom.nnue", result.stdout)
        self.assertIn("custom.metadata.json", result.stdout)


class TelemetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_loaded_lines_are_parsed_for_both_trainers(self):
        event = self.core.parse_progress("loaded 2172420 rows (18.2 active, pad 32) in 4.9s")
        self.assertEqual(event["kind"], "loaded")
        self.assertEqual(event["rows"], 2172420)
        self.assertAlmostEqual(event["seconds"], 4.9)
        self.assertAlmostEqual(event["rows_per_second"], 2172420 / 4.9, places=1)
        legacy = self.core.parse_progress("loaded 400 rows in 0.4s")
        self.assertEqual(legacy["kind"], "loaded")
        self.assertEqual(legacy["rows"], 400)

    def test_update_from_log_events_persists_the_histories(self):
        with tempfile.TemporaryDirectory() as directory:
            run = self.core.Run(directory=Path(directory))
            events = [
                self.core.parse_progress(
                    "epoch 1/10 train_loss 2.18769 val_loss 1.83720 val_mae_cp 183.7 time 138.9s"
                ),
                self.core.parse_progress(
                    "epoch 2/10 train_loss 2.00000 val_loss 1.70000 val_mae_cp 170.2 time 140.1s"
                ),
                self.core.parse_progress("loaded 2172420 rows (18.2 active, pad 32) in 4.9s"),
            ]
            run.update_from_log_events(events)
            progress = run.state["progress"]
            self.assertEqual(progress["history"], [183.7, 170.2])
            self.assertEqual(progress["train_loss_history"], [2.18769, 2.0])
            self.assertEqual(progress["val_loss_history"], [1.8372, 1.7])
            self.assertEqual(progress["seconds_history"], [138.9, 140.1])
            self.assertEqual(progress["rows_loaded"], 2172420)
            self.assertAlmostEqual(progress["rows_per_second"], 2172420 / 4.9, places=1)

    def test_eta_summary_and_duration_formatting(self):
        progress = {
            "epoch": 2,
            "epochs": 10,
            "val_mae_cp": 142.1,
            "seconds_history": [100.0, 110.0],
            "rows_per_second": 15000.0,
        }
        self.assertAlmostEqual(self.core.estimate_eta(progress), 840.0)
        summary = self.core.progress_summary(progress)
        self.assertIn("epoch 2/10", summary)
        self.assertIn("val MAE 142.1 cp", summary)
        self.assertIn("ETA 14m 00s", summary)
        self.assertIn("15,000 rows/s", summary)
        self.assertIsNone(self.core.estimate_eta({"epoch": 1, "epochs": 5}))
        self.assertEqual(self.core.estimate_eta({"epoch": 5, "epochs": 5}), 0.0)
        self.assertEqual(self.core.format_duration(45), "45s")
        self.assertEqual(self.core.format_duration(272), "4m 32s")
        self.assertEqual(self.core.format_duration(3900), "1h 05m")

    def test_chart_series_and_bounds(self):
        progress = {
            "train_loss_history": [2.0, 1.5],
            "val_loss_history": [1.8, 1.4],
            "history": [180.0, 140.0],
        }
        series = self.core.chart_series(progress)
        self.assertEqual([item["name"] for item in series], ["Train loss", "Val loss", "Val MAE"])
        self.assertEqual([item["axis"] for item in series], ["loss", "loss", "mae"])
        self.assertEqual(series[0]["values"], [2.0, 1.5])
        # Losses and centipawn error are drawn against separate scales.
        self.assertEqual(self.core.chart_bounds(series, "loss"), (1.4, 2.0))
        self.assertEqual(self.core.chart_bounds(series, "mae"), (140.0, 180.0))
        self.assertEqual(self.core.chart_bounds(series), (1.4, 180.0))
        flat = [{"name": "flat", "color": "#000000", "axis": "mae", "values": [5.0, 5.0]}]
        self.assertEqual(self.core.chart_bounds(flat), (4.0, 6.0))
        self.assertIsNone(self.core.chart_bounds([]))
        self.assertIsNone(self.core.chart_bounds(series, "missing"))
        self.assertEqual(self.core.chart_series({}), [])

    def test_log_line_matches_filters(self):
        line = "epoch 1/10 train_loss 2.18769 val_loss 1.83720 val_mae_cp 183.7 time 138.9s"
        self.assertTrue(self.core.log_line_matches(line, "", False))
        self.assertTrue(self.core.log_line_matches(line, "EPOCH", False))
        self.assertFalse(self.core.log_line_matches(line, "traceback", False))
        self.assertFalse(self.core.log_line_matches(line, "", True))
        self.assertTrue(self.core.log_line_matches("ValueError: bad network", "", True))
        self.assertTrue(self.core.log_line_matches("run failed with exit code 1", "", True))


class RunListTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    @staticmethod
    def _run(root: Path, name: str, **state):
        directory = root / name
        directory.mkdir()
        return RunListTests.core.Run(directory=directory, config={}, state=state)

    def test_filter_runs_matches_name_kind_and_status(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = [
                self._run(root, "20260918-train-koi", kind="train", status="completed"),
                self._run(root, "20260918-data-datagen", kind="data", status="running"),
                self._run(root, "20260918-train-torch", kind="train", status="failed"),
            ]
            self.assertEqual(len(self.core.filter_runs(runs)), 3)
            self.assertEqual(len(self.core.filter_runs(runs, "train")), 2)
            self.assertEqual(len(self.core.filter_runs(runs, "FAILED")), 1)
            self.assertEqual(len(self.core.filter_runs(runs, "datagen")), 1)
            self.assertEqual(self.core.filter_runs(runs, "missing"), [])

    def test_sort_runs_by_columns(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runs = [
                self._run(root, "b-run", status="failed", progress={"val_mae_cp": 150.0}),
                self._run(root, "a-run", status="completed", progress={"val_mae_cp": 120.0}),
                self._run(root, "c-run", status="running"),
            ]
            self.assertEqual([run.directory.name for run in self.core.sort_runs(runs, "run")],
                             ["a-run", "b-run", "c-run"])
            self.assertEqual([run.directory.name for run in self.core.sort_runs(runs, "run", True)],
                             ["c-run", "b-run", "a-run"])
            self.assertEqual([run.directory.name for run in self.core.sort_runs(runs, "status")],
                             ["a-run", "b-run", "c-run"])
            # A run without a validation MAE sorts last.
            self.assertEqual([run.directory.name for run in self.core.sort_runs(runs, "val_mae")],
                             ["a-run", "b-run", "c-run"])

    def test_duration_and_failure_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = self._run(
                root,
                "timed-run",
                created="2026-09-18T00:00:00Z",
                finished="2026-09-18T00:01:30Z",
            )
            self.assertEqual(self.core.run_duration_seconds(run.state), 90.0)
            self.assertIsNone(self.core.run_duration_seconds({"created": "2026-09-18T00:00:00Z"}))
            self.assertIsNone(self.core.run_duration_seconds({"created": "not-a-date", "finished": "x"}))
            self.assertEqual(self.core.failure_lines(run), [])
            (run.directory / "train.err").write_text("first\n\nsecond\nthird\n", encoding="utf-8")
            self.assertEqual(self.core.failure_lines(run), ["first", "second", "third"])
            self.assertEqual(self.core.failure_lines(run, limit=2), ["second", "third"])

    def test_run_detail_lines_include_status_and_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = self._run(
                root,
                "20260918-train-koi",
                kind="train",
                backend="koi",
                status="failed",
                exit_code=1,
                created="2026-09-18T00:00:00Z",
                finished="2026-09-18T00:01:30Z",
                progress={"epoch": 3, "epochs": 10, "val_mae_cp": 141.9},
            )
            (run.directory / "train.err").write_text("Traceback: boom\n", encoding="utf-8")
            detail = "\n".join(self.core.run_detail_lines(run))
            self.assertIn("status: failed", detail)
            self.assertIn("exit code: 1", detail)
            self.assertIn("duration: 1m 30s", detail)
            self.assertIn("val MAE: 141.9 cp", detail)
            self.assertIn("network: not written", detail)
            self.assertIn("Traceback: boom", detail)


if __name__ == "__main__":
    unittest.main()
