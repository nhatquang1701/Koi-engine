import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class NnueWrapperTest(unittest.TestCase):
    def test_training_wrapper_exposes_the_koi_nnue_cli(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "train_nnue.py"), "--help"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--output-network", result.stdout)
        self.assertIn("--backend", result.stdout)

    def test_export_wrapper_exposes_the_same_versioned_cli(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "export_nnue.py"), "--help"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--output-network", result.stdout)
        self.assertIn("--manifest", result.stdout)


if __name__ == "__main__":
    unittest.main()
