import re
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARAMETERS = ROOT / "src" / "koi" / "evaluation_parameters.hpp"
TOOL = ROOT / "tools" / "tune_eval.py"
CORPUS = "4k3/8/8/8/8/8/P7/4K3 w - - 0 1,1-0\n"


class TuneEvalTest(unittest.TestCase):
    def test_output_tracks_every_canonical_parameter(self):
        source = PARAMETERS.read_text(encoding="utf-8")
        version = re.search(r'string_view version = "([^"]+)"', source).group(1)
        fields = re.findall(r"\bint\s+(\w+)\s*=\s*(-?\d+);", source)
        result = subprocess.run(
            [sys.executable, str(TOOL), "--input", "-"],
            cwd=ROOT,
            input=CORPUS,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            f'kTunedEvaluationParameterVersion = "{version}"', result.stdout
        )
        for name, value in fields:
            self.assertIn(
                f"kTunedEvaluation_{name} = {value};", result.stdout
            )


if __name__ == "__main__":
    unittest.main()
