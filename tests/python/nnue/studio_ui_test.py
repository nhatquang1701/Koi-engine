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

        def fake_gate(net, **kwargs):
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

        def fake_gate(net, **kwargs):
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


class SettingsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_defaults_and_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "settings.json"
            defaults = self.core.read_settings(path)
            self.assertEqual(defaults["theme"], "light")
            self.assertFalse(defaults["auto_adopt"])
            self.assertEqual(defaults["ab_games"], 20)
            settings = dict(defaults)
            settings.update({"engine_directory": "D:/eng", "auto_adopt": True, "theme": "dark"})
            self.core.write_settings(settings, path)
            loaded = self.core.read_settings(path)
            self.assertEqual(loaded["engine_directory"], "D:/eng")
            self.assertTrue(loaded["auto_adopt"])
            self.assertEqual(loaded["theme"], "dark")
            raw = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(raw["schema"], self.core.SETTINGS_SCHEMA)

    def test_malformed_settings_fall_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "settings.json"
            path.write_text("{not json", encoding="utf-8")
            self.assertEqual(self.core.read_settings(path), self.core.default_settings())


class AdoptionPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_gate_failure_skips(self):
        for gate in (None, {}, {"error": "boom"}, {"rejected": True}, {"positions": 0}):
            decision = self.core.adoption_decision(gate, None, None)
            self.assertEqual(decision["decision"], "skip")

    def test_no_installed_network_adopts_after_gate(self):
        decision = self.core.adoption_decision({"positions": 64, "matches": 64}, None, None)
        self.assertEqual(decision["decision"], "adopt")

    def test_installed_network_requires_a_comparison(self):
        installed = Path("D:/eng/koi.nnue")
        gate = {"positions": 64}
        self.assertEqual(self.core.adoption_decision(gate, None, installed)["decision"], "skip")
        self.assertEqual(
            self.core.adoption_decision(gate, {"error": "no report"}, installed)["decision"],
            "skip",
        )
        self.assertEqual(
            self.core.adoption_decision(
                gate, {"verdict": "candidate-weaker"}, installed
            )["decision"],
            "skip",
        )
        for verdict in ("candidate-stronger", "inconclusive"):
            self.assertEqual(
                self.core.adoption_decision(gate, {"verdict": verdict}, installed)["decision"],
                "adopt",
            )
        self.assertEqual(
            self.core.adoption_decision(gate, {"verdict": "surprising"}, installed)["decision"],
            "skip",
        )


class AdoptionRegistryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_adopt_records_and_reverts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            engine = root / "engine"
            engine.mkdir()
            old = engine / "koi.nnue"
            old.write_bytes(b"old-net")
            new = root / "candidate.nnue"
            new.write_bytes(b"new-net")
            registry = root / "adoptions.json"
            info = self.core.adopt_net(
                new,
                engine,
                evidence={"gate": {"positions": 64}, "net_match_verdict": "candidate-stronger"},
                registry_path=registry,
            )
            self.assertEqual(Path(info["installed"]).read_bytes(), b"new-net")
            self.assertIsNotNone(info["backup"])
            records = self.core.load_adoptions(registry)
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0]["net_match_verdict"], "candidate-stronger")
            self.assertIsNone(records[0]["reverted_utc"])
            reverted = self.core.revert_adoption(registry_path=registry)
            self.assertEqual(Path(reverted["reverted"]).read_bytes(), b"old-net")
            self.assertIsNotNone(self.core.load_adoptions(registry)[0]["reverted_utc"])
            self.assertEqual(
                self.core.revert_adoption(registry_path=registry)["error"],
                "no adoption is available to revert",
            )

    def test_missing_backup_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            registry = root / "adoptions.json"
            self.core.write_json(
                registry,
                {
                    "schema": self.core.ADOPTION_SCHEMA,
                    "adoptions": [
                        {
                            "installed_path": str(root / "engine" / "koi.nnue"),
                            "backup_path": str(root / "missing.bak"),
                            "reverted_utc": None,
                        }
                    ],
                },
            )
            result = self.core.revert_adoption(registry_path=registry)
            self.assertIn("missing", result["error"])


class NetMatchCommandTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_command_uses_the_reference_wrapper(self):
        candidate = Path("D:/runs/a/net.nnue")
        opponent = Path("D:/engine/koi.nnue")
        command = self.core.net_match_command(candidate, opponent, games=12, nodes=5000, threads=2)
        text = " ".join(command)
        self.assertIn("net_match.ps1", text)
        self.assertIn("-NnueNet", command)
        self.assertIn("-OpponentNet", command)
        self.assertEqual(command[command.index("-Games") + 1], "12")
        self.assertEqual(command[command.index("-Nodes") + 1], "5000")
        self.assertEqual(command[command.index("-Threads") + 1], "2")
        self.assertTrue(command[command.index("-ReportPath") + 1].endswith("net-match.json"))

    def test_running_engines_returns_a_list(self):
        self.assertIsInstance(self.core.running_engines(), list)


class GuiAdoptionWiringTests(unittest.TestCase):
    def test_gui_exposes_the_auto_adopt_controls(self):
        source = (TOOLS_NNUE / "koi_nnue_studio.py").read_text(encoding="utf-8")
        self.assertIn("Auto-adopt", source)
        self.assertIn("Adopt best run", source)
        self.assertIn("_adoption_worker", source)
        self.assertIn("running_engines", source)
        self.assertIn("_save_settings", source)
        self.assertIn("Revert last adoption", source)


class ThemeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_palettes_expose_the_contract_keys(self):
        keys = {
            "bg",
            "surface",
            "border",
            "text",
            "muted",
            "accent",
            "danger",
            "chart_bg",
            "chart_grid",
            "series",
        }
        for name in ("light", "dark"):
            colors = self.core.palette(name)
            self.assertTrue(keys <= set(colors), f"{name} palette is missing keys")
            self.assertGreaterEqual(len(colors["series"]), 3)
            self.assertTrue(all(isinstance(color, str) and color.startswith("#") for color in colors["series"]))

    def test_unknown_theme_falls_back_to_light(self):
        self.assertEqual(self.core.palette("neon"), self.core.palette("light"))
        self.assertEqual(self.core.palette(None), self.core.palette("light"))

    def test_theme_round_trips_through_settings(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "settings.json"
            self.assertEqual(self.core.read_settings(path)["theme"], "light")
            self.core.write_settings({"theme": "dark"}, path)
            self.assertEqual(self.core.read_settings(path)["theme"], "dark")

    def test_configure_styles_applies_the_palette(self):
        if importlib.util.find_spec("tkinter") is None:
            self.skipTest("tkinter is unavailable")
        import tkinter as tk
        import tkinter.ttk as ttk

        try:
            root = tk.Tk()
        except tk.TclError:
            self.skipTest("no display available")
        try:
            style = ttk.Style(root)
            colors = self.core.palette("dark")
            self.core.configure_styles(style, colors)
            self.assertEqual(style.lookup("TFrame", "background"), colors["bg"])
            self.assertEqual(style.lookup("Muted.TLabel", "foreground"), colors["muted"])
            self.assertEqual(style.lookup("Accent.TButton", "foreground"), colors["accent"])
            self.assertEqual(style.lookup("Danger.TButton", "foreground"), colors["danger"])
            self.assertEqual(style.lookup("Treeview", "fieldbackground"), colors["surface"])
            self.assertEqual(style.lookup("TProgressbar", "background"), colors["accent"])
        finally:
            root.destroy()


class ChartHoverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_hover_index_maps_x_to_the_nearest_epoch(self):
        # Plot from x=50, width 100, five epochs: the nearest epoch changes at
        # ratio 0.125, 0.375, 0.625 and 0.875 of the plot width.
        self.assertEqual(self.core.chart_hover_index(50, 50, 100, 5), 0)
        self.assertEqual(self.core.chart_hover_index(62, 50, 100, 5), 0)
        self.assertEqual(self.core.chart_hover_index(63, 50, 100, 5), 1)
        self.assertEqual(self.core.chart_hover_index(150, 50, 100, 5), 4)

    def test_hover_index_rejects_coordinates_outside_the_plot(self):
        self.assertIsNone(self.core.chart_hover_index(20, 50, 100, 5))
        self.assertIsNone(self.core.chart_hover_index(400, 50, 100, 5))
        self.assertIsNone(self.core.chart_hover_index(50, 50, 100, 0))
        self.assertIsNone(self.core.chart_hover_index(50, 50, 0, 5))

    def test_single_epoch_hover_always_hits_index_zero(self):
        self.assertEqual(self.core.chart_hover_index(50, 50, 100, 1), 0)
        self.assertEqual(self.core.chart_hover_index(150, 50, 100, 1), 0)


class CancelTokenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_token_starts_clear_and_cancels_once(self):
        token = self.core.CancelToken()
        self.assertFalse(token.is_cancelled())
        token.cancel()
        self.assertTrue(token.is_cancelled())
        token.cancel()  # idempotent

    def test_run_process_stops_a_cancelled_command(self):
        token = self.core.CancelToken()
        token.cancel()
        result = self.core.run_process(
            [sys.executable, "-c", "import time; time.sleep(30)"], timeout=60, cancel=token
        )
        self.assertTrue(result["cancelled"])
        self.assertNotEqual(result["returncode"], 0)

    def test_run_process_streams_lines_and_reports_success(self):
        lines: list[str] = []
        result = self.core.run_process(
            [sys.executable, "-c", "print('hello'); print('world')"],
            timeout=60,
            on_line=lines.append,
        )
        self.assertEqual(result["returncode"], 0)
        self.assertFalse(result["cancelled"])
        self.assertFalse(result["timed_out"])
        self.assertTrue(any("hello" in line for line in lines))
        self.assertTrue(any("world" in line for line in lines))

    def test_terminate_process_tree_ignores_missing_pids(self):
        self.core.terminate_process_tree(999_999_999)  # must not raise


class GuiPhaseThreeWiringTests(unittest.TestCase):
    def test_gui_exposes_the_presentation_controls(self):
        source = (TOOLS_NNUE / "koi_nnue_studio.py").read_text(encoding="utf-8")
        for marker in (
            "_apply_dpi_awareness",
            "_scrollable_tab",
            "_apply_theme",
            "_on_chart_motion",
            "_export_chart",
            "_cancel_validation",
            "_apply_runs",
            "install_error",
            "Ctrl+T",
            "Ctrl+R",
            "F5",
            "Escape",
        ):
            self.assertIn(marker, source, f"{marker} is missing from the GUI")

    def test_gui_selftest_applies_both_themes(self):
        source = (TOOLS_NNUE / "koi_nnue_studio.py").read_text(encoding="utf-8")
        selftest = source.split("def gui_selftest", 1)[1]
        self.assertIn('_apply_theme("dark")', selftest)
        self.assertIn('_apply_theme("light")', selftest)


if __name__ == "__main__":
    unittest.main()
