import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
REPORT_SCRIPT = REPOSITORY_ROOT / "tools" / "measurement" / "strength_report.py"


def run_report(*arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(REPORT_SCRIPT), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


def profile(*, threads: int = 1, include_pv: bool = True) -> dict:
    positions = [
        {
            "id": "positional_01",
            "fen": "4k3/8/8/8/8/2N5/P7/4K3 w - - 0 1",
            "limits": {"depth": 1},
            "hash_mb": 512,
            "hash_state": "cold",
            "threads": threads,
            "speed": 100,
            "score_cp": 20,
            "nodes": 10,
            "qnodes": 4,
            "tt_hits": 1,
            "nps": 1000,
            "elapsed_ms": 14,
            "expected_move": "c3b5",
            "accepted_moves": ["c3b5", "c3d5"],
        },
        {
            "id": "endgame_01",
            "fen": "4k3/8/8/8/8/8/P7/4K3 w - - 0 1",
            "limits": {"depth": 1},
            "hash_mb": 512,
            "hash_state": "cold",
            "threads": threads,
            "speed": 100,
            "score_cp": -10,
            "nodes": 20,
            "qnodes": 8,
            "tt_hits": 2,
            "nps": 2000,
            "elapsed_ms": 16,
            "expected_move": "e1d1",
            "accepted_moves": ["e1d1"],
        },
    ]
    if include_pv:
        positions[0]["pv"] = ["c3b5"]
        positions[1]["pv"] = ["e1f1"]
    return {
        "schema": "koi-bench-profile-v1",
        "engine": "Koi Engine",
        "build": "Koi Engine test",
        "suite": "optional_strength",
        "warm_hash": False,
        "hash_state": "cold",
        "timed": True,
        "hash_mb": 512,
        "threads": threads,
        "speed": 100,
        "positions": positions,
    }


class StrengthReportTests(unittest.TestCase):
    def test_category_aggregation_and_deterministic_output(self):
        with tempfile.TemporaryDirectory(prefix="koi strength report ") as temporary:
            root = Path(temporary)
            profile_path = root / "profile.json"
            first_path = root / "first.json"
            second_path = root / "second.json"
            profile_path.write_text(json.dumps(profile()), encoding="utf-8")

            first = run_report(
                "--profile",
                str(profile_path),
                "--expected-suite",
                "optional_strength",
                "--output",
                str(first_path),
            )
            second = run_report(
                "--profile",
                str(profile_path),
                "--expected-suite",
                "optional_strength",
                "--output",
                str(second_path),
            )

            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(first.stdout, "")
            self.assertEqual(first_path.read_bytes(), second_path.read_bytes())
            report = json.loads(first_path.read_text(encoding="utf-8"))

        self.assertEqual(report["schema"], "koi-strength-report-v1")
        self.assertEqual(report["position_count"], 2)
        self.assertEqual(report["expected_move_count"], 2)
        self.assertEqual(report["accepted_count"], 1)
        self.assertEqual(report["mismatch_ids"], ["endgame_01"])
        self.assertEqual(report["categories"]["positional"]["position_count"], 1)
        self.assertEqual(report["categories"]["endgame"]["accepted_count"], 0)
        self.assertEqual(report["total_nodes"], 30)
        self.assertEqual(report["total_qnodes"], 12)

    def test_malformed_json_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="koi strength malformed ") as temporary:
            root = Path(temporary)
            profile_path = root / "broken.json"
            output_path = root / "report.json"
            profile_path.write_text("{not json", encoding="utf-8")
            completed = run_report(
                "--profile",
                str(profile_path),
                "--expected-suite",
                "optional_strength",
                "--output",
                str(output_path),
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("invalid JSON", completed.stderr)

    def test_missing_pv_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix="koi strength missing pv ") as temporary:
            root = Path(temporary)
            profile_path = root / "profile.json"
            output_path = root / "report.json"
            profile_path.write_text(json.dumps(profile(include_pv=False)), encoding="utf-8")
            completed = run_report(
                "--profile",
                str(profile_path),
                "--expected-suite",
                "optional_strength",
                "--output",
                str(output_path),
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("pv", completed.stderr)

    def test_mixed_options_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="koi strength mixed ") as temporary:
            root = Path(temporary)
            first_path = root / "first.json"
            second_path = root / "second.json"
            output_path = root / "report.json"
            first_path.write_text(json.dumps(profile()), encoding="utf-8")
            second_path.write_text(json.dumps(profile(threads=2)), encoding="utf-8")
            completed = run_report(
                "--profile",
                str(first_path),
                "--profile",
                str(second_path),
                "--expected-suite",
                "optional_strength",
                "--output",
                str(output_path),
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("threads", completed.stderr)


if __name__ == "__main__":
    unittest.main()
