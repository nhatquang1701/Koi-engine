"""Tests for the classical term tuner and the feature CSV it consumes.

The recovery case builds a synthetic corpus whose target is a known linear
combination of the term columns, so the fit has to return those scales.  The
end-to-end case runs the real ``koi-eval-features`` tool when it has been built
and checks the CSV invariants the tuner relies on.
"""

from __future__ import annotations

import csv
import json
import pathlib
import random
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
MEASUREMENT = REPO_ROOT / "tools" / "measurement"
if str(MEASUREMENT) not in sys.path:
    sys.path.insert(0, str(MEASUREMENT))

import tune_classical  # noqa: E402

TERMS = tune_classical.TERMS


def synthetic_rows(count: int = 240, seed: int = 20260918) -> list[dict[str, float]]:
    rng = random.Random(seed)
    rows: list[dict[str, float]] = []
    for _ in range(count):
        terms = {term: rng.uniform(-5.0, 5.0) for term in TERMS}
        target = terms["material"] + 2.0 * terms["mobility"] - 0.5 * terms["tempo"]
        rows.append({**terms, "total": float(sum(terms.values())), "cp": target})
    return rows


def write_features_csv(path: pathlib.Path, rows: list[dict[str, float]]) -> None:
    fieldnames = ["fen", "cp", "phase", *TERMS, "total"]
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for index, row in enumerate(rows):
            writer.writerow({"fen": f"8/8/8/8/8/8/8/K6k w - - 0 {index}", "phase": 0, **row})


class RecoveryTests(unittest.TestCase):
    def test_recovers_the_known_scales(self) -> None:
        report = tune_classical.fit_scales(synthetic_rows(), "cp", ridge=1e-9)
        scales = report["scales"]
        self.assertAlmostEqual(scales["material"], 1.0, places=5)
        self.assertAlmostEqual(scales["mobility"], 2.0, places=5)
        self.assertAlmostEqual(scales["tempo"], -0.5, places=5)
        self.assertAlmostEqual(scales["king_safety"], 0.0, places=5)
        self.assertLess(report["fitted"]["mae"], report["current"]["mae"])
        self.assertGreater(report["fitted"]["r2"], 0.99)

    def test_flags_a_degenerate_corpus(self) -> None:
        rows = synthetic_rows(count=5)
        with self.assertRaises(tune_classical.TuningError):
            tune_classical.fit_scales(rows, "cp")


class ReportTests(unittest.TestCase):
    def test_header_and_report_symbols(self) -> None:
        report = tune_classical.fit_scales(synthetic_rows(), "cp", ridge=1e-9)
        header = tune_classical.generated_header(report, "deadbeef")
        self.assertIn("kTunedClassicalMetadataFormatVersion", header)
        self.assertIn("kTunedClassicalScaleMobility", header)
        self.assertIn("kTunedClassicalScaleKingSafety", header)
        self.assertIn("deadbeef", header)
        self.assertIn("NOT adopted", header)

    def test_cli_writes_header_and_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            csv_path = root / "features.csv"
            write_features_csv(csv_path, synthetic_rows())
            header_path = root / "tuned.h"
            report_path = root / "report.json"
            code = tune_classical.main(
                [
                    "--input",
                    str(csv_path),
                    "--header-out",
                    str(header_path),
                    "--report-out",
                    str(report_path),
                ]
            )
            self.assertEqual(code, 0)
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema"], "koi-classical-tuning-report-v1")
            self.assertEqual(report["target"], "cp")
            self.assertEqual(report["adopted"], False)
            self.assertEqual(report["rows"], 240)
            self.assertEqual(set(report["scales"]), set(TERMS))
            self.assertTrue(header_path.read_text(encoding="utf-8").startswith("#pragma once"))


class EmittedCsvTests(unittest.TestCase):
    """The real dump must satisfy the contract the tuner assumes."""

    def test_feature_csv_round_trip(self) -> None:
        executable = REPO_ROOT / "build" / "release" / "koi-eval-features.exe"
        if not executable.exists():
            self.skipTest("koi-eval-features is not built")
        corpus = (
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1;24;e2e4\n"
            "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4;-31;f7f5\n"
        )
        result = subprocess.run(
            [str(executable)], input=corpus, capture_output=True, text=True, check=False
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = [line for line in result.stdout.splitlines() if line]
        self.assertEqual(lines[0].split(",")[0], "fen")
        self.assertEqual([int(value) for value in lines[1].split(",")[1:2]], [24])
        self.assertEqual([int(value) for value in lines[2].split(",")[1:2]], [-31])
        for line in lines[1:]:
            fields = line.split(",")
            self.assertEqual(len(fields), 2 + 1 + len(TERMS) + 1)
            values = [int(value) for value in fields[2:]]
            terms = dict(zip(TERMS, values[1:]))
            self.assertEqual(sum(terms.values()), values[-1], line)


if __name__ == "__main__":
    unittest.main()
