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


if __name__ == "__main__":
    unittest.main()
