import re
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
PARAMETERS = ROOT / "src" / "koi" / "evaluation_parameters.hpp"
TOOL = ROOT / "tools" / "measurement" / "tune_eval.py"
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

    def test_split_corpora_emit_independent_hashes_and_counts(self):
        corpora = {
            "train": "4k3/8/8/8/8/8/P7/4K3 w - - 0 1,1-0\n",
            "validation": "4k3/8/8/8/8/8/1P6/4K3 w - - 0 1,1/2-1/2\n",
            "holdout": "4k3/8/8/8/8/8/2P5/4K3 w - - 0 1,0-1\n",
        }
        with tempfile.TemporaryDirectory() as temporary:
            paths = {}
            for name, text in corpora.items():
                path = Path(temporary) / f"{name}.csv"
                path.write_text(text, encoding="utf-8")
                paths[name] = path
            result = subprocess.run(
                [
                    sys.executable,
                    str(TOOL),
                    "--train",
                    str(paths["train"]),
                    "--validation",
                    str(paths["validation"]),
                    "--holdout",
                    str(paths["holdout"]),
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertEqual(result.returncode, 0, result.stderr)
        for name, text in corpora.items():
            digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
            capitalized = name.capitalize()
            self.assertIn(
                f'kTunedEvaluation{capitalized}CorpusSha256 = "{digest}"',
                result.stdout,
            )
            self.assertIn(
                f"kTunedEvaluation{capitalized}PositionCount = 1;",
                result.stdout,
            )


if __name__ == "__main__":
    unittest.main()
