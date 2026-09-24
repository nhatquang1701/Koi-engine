"""Unit tests for the Koi NNUE Studio (tools/nnue).

The tests exercise the headless contract the GUI is built on:

* the trainer progress parser used for live charts,
* the preset table and default configuration,
* ``--list-backends`` / ``--dry-run`` command construction,
* an end-to-end tiny training run (``--selftest --no-gate``) when torch is
  importable, proving the Python trainer and container writer are wired up.

The GUI smoke test (``--gui-selftest``) runs only when tkinter is present.
"""

from __future__ import annotations

import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
TOOLS_NNUE = REPO_ROOT / "tools" / "nnue"
STUDIO = TOOLS_NNUE / "koi_nnue_studio.py"

try:
    import torch  # noqa: F401

    TORCH_AVAILABLE = True
except Exception:  # pragma: no cover - optional dependency
    TORCH_AVAILABLE = False

try:
    import tkinter  # noqa: F401

    TK_AVAILABLE = True
except Exception:  # pragma: no cover - optional dependency
    TK_AVAILABLE = False


def load_studio_core():
    spec = importlib.util.spec_from_file_location("koi_studio_core", TOOLS_NNUE / "studio_core.py")
    module = importlib.util.module_from_spec(spec)
    # dataclasses needs the module registered while its annotations resolve.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def load_bullet_backend():
    for path in (TOOLS_NNUE, TOOLS_NNUE / "backends"):
        if str(path) not in sys.path:
            sys.path.insert(0, str(path))
    import bullet_backend  # noqa: PLC0415  (needs the sys.path entries above)

    return bullet_backend


def run_studio(*arguments: str, timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(STUDIO), *arguments],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


class ProgressParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core = load_studio_core()

    def test_epoch_line_is_parsed(self):
        event = self.core.parse_progress("epoch 3/10 train_loss 1.23 val_loss 1.11 val_mae_cp 141.2 time 17.5s")
        self.assertIsNotNone(event)
        self.assertEqual(event["kind"], "epoch")
        self.assertEqual(event["epoch"], 3)
        self.assertEqual(event["epochs"], 10)
        self.assertAlmostEqual(event["val_mae_cp"], 141.2)

    def test_quantization_and_selection_lines_are_parsed(self):
        quantization = self.core.parse_progress("quantization v3 s1=7 s2=7 k3=4 val_mae_cp 122.9")
        self.assertEqual(quantization["kind"], "quantization")
        self.assertEqual(quantization["val_mae_cp"], 122.9)
        selected = self.core.parse_progress("selected v3 shifts s1=7 s2=7 k3=4 val_mae_cp 122.9")
        self.assertEqual(selected["kind"], "selected")

    def test_wrote_line_reports_the_network(self):
        event = self.core.parse_progress("wrote C:/tmp/net.nnue (501011 bytes) and C:/tmp/net.metadata.json")
        self.assertEqual(event["kind"], "wrote")
        self.assertTrue(str(event["net"]).endswith("net.nnue"))

    def test_unrelated_lines_are_ignored(self):
        self.assertIsNone(self.core.parse_progress("opening artifacts/training/labels.txt"))

    def test_loaded_line_reports_throughput(self):
        event = self.core.parse_progress("loaded 1200002 rows in 4.2s")
        self.assertEqual(event["kind"], "loaded")
        self.assertEqual(event["rows"], 1200002)
        self.assertAlmostEqual(event["rows_per_second"], 1200002 / 4.2, places=1)

    def test_presets_have_ordered_effort(self):
        presets = self.core.presets()
        self.assertEqual(set(presets), {"quick", "standard", "thorough"})
        self.assertLess(presets["quick"]["epochs"], presets["standard"]["epochs"])
        self.assertLessEqual(presets["standard"]["epochs"], presets["thorough"]["epochs"])
        for preset in presets.values():
            self.assertIn("batch_size", preset)
            self.assertIn("learning_rate", preset)


class StudioCliTests(unittest.TestCase):
    def test_list_backends_reports_the_trainers(self):
        result = run_studio("--list-backends", timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("koi", result.stdout)
        self.assertIn("torch", result.stdout)
        self.assertIn("available", result.stdout)

    def test_dry_run_builds_a_v4_trainer_command(self):
        result = run_studio("--dry-run", "--preset", "quick", timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("train_nnue_koi.py", result.stdout)
        self.assertIn("--hidden-units", result.stdout)
        self.assertIn("--output-shifts", result.stdout)

    def test_dry_run_can_target_the_legacy_v3_trainer(self):
        result = run_studio(
            "--dry-run", "--preset", "quick", "--backend", "torch", timeout=120
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("train_nnue_sf.py", result.stdout)
        self.assertIn("--format v3", result.stdout)

    def test_dry_run_targets_the_bullet_gpu_trainer(self):
        result = run_studio("--dry-run", "--preset", "quick", "--backend", "bullet", timeout=120)
        backend = load_bullet_backend().BulletBackend()
        if backend.available():
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("run_bullet.py", result.stdout)
        else:
            self.assertEqual(result.returncode, 2)
            self.assertIn("bullet", result.stderr)

    @unittest.skipUnless(TK_AVAILABLE, "tkinter is unavailable")
    def test_gui_selftest_constructs_the_window(self):
        if os.name != "nt" and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
            self.skipTest("no display available")
        result = run_studio("--gui-selftest", timeout=120)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("PASS gui construction", result.stdout)


class BulletBackendTests(unittest.TestCase):
    def test_build_command_targets_the_wrapper(self):
        bullet = load_bullet_backend()
        backend = bullet.BulletBackend()
        if not backend.available():
            self.skipTest(backend.unavailable_reason() or "bullet backend is unavailable")
        command = backend.build_command(
            Path("run"),
            {
                "net_name": "custom.nnue",
                "corpus": "labels.txt",
                "bullet_superbatches": 5,
                "batch_size": 4096,
                "threads": 2,
            },
        )
        self.assertIn("run_bullet.py", command[1])
        self.assertIn("custom.nnue", command)
        self.assertIn("--superbatches", command)
        self.assertIn("5", command)
        self.assertIn("--batch", command)
        self.assertIn("4096", command)


@unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is required for the training selftest")
class SelftestTests(unittest.TestCase):
    def test_selftest_trains_a_tiny_network(self):
        if not STUDIO.exists():
            self.skipTest("koi_nnue_studio.py is missing")
        positions = [
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
        ]
        with tempfile.TemporaryDirectory(prefix="koi-studio-test-") as directory:
            corpus = Path(directory) / "tiny-corpus.txt"
            rows = []
            for index in range(400):
                rows.append(f"{positions[index % len(positions)]};{index % 40 - 20}")
            corpus.write_text("\n".join(rows) + "\n", encoding="utf-8")

            result = run_studio(
                "--selftest",
                "--corpus",
                str(corpus),
                "--rows",
                "400",
                "--epochs",
                "1",
                "--threads",
                "2",
                "--no-gate",
                timeout=900,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("wrote", result.stdout)
        self.assertIn(".nnue", result.stdout)


if __name__ == "__main__":
    unittest.main()
