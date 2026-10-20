import json
import tempfile
import unittest
from pathlib import Path

from tools.measurement import spsa_search


ROOT = Path(__file__).resolve().parents[3]


class DefaultManifestTest(unittest.TestCase):
    def test_every_default_pattern_matches_exactly_once(self):
        for parameter in spsa_search.DEFAULT_PARAMETERS:
            with self.subTest(parameter=parameter.name):
                text = (ROOT / parameter.path).read_text(encoding="utf-8")
                pattern = spsa_search._compiled(parameter)
                self.assertEqual(len(pattern.findall(text)), 1)
                match = pattern.search(text)
                self.assertEqual(int(match.group(1)), parameter.value)

    def test_manifest_round_trip(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(spsa_search.default_manifest()), encoding="utf-8")
            loaded = spsa_search.load_manifest(path)
        self.assertEqual([entry.name for entry in loaded],
                         [entry.name for entry in spsa_search.DEFAULT_PARAMETERS])


class StagingTest(unittest.TestCase):
    def test_stage_variant_rewrites_only_touched_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "variant"
            values = {parameter.name: parameter.value
                      for parameter in spsa_search.DEFAULT_PARAMETERS}
            with self.assertRaises(spsa_search.TuningError):
                # An out-of-range value is rejected before any file is written.
                spsa_search.stage_variant(
                    ROOT, spsa_search.DEFAULT_PARAMETERS,
                    {**values, "razor_margin_per_depth_squared": 9999}, output)
            self.assertFalse(output.exists())
            spsa_search.stage_variant(
                ROOT, spsa_search.DEFAULT_PARAMETERS,
                {**values, "razor_margin_per_depth_squared": 150}, output)
            staged = (output / "src/koi/detail/search_constants.hpp").read_text(
                encoding="utf-8")
            self.assertIn("kRazorMarginPerDepthSquared = 150", staged)
            self.assertFalse((output / "src/koi/nnue.cpp").exists())

    def test_apply_values_rejects_ambiguous_patterns(self):
        parameter = spsa_search.Parameter(
            "ambiguous", "x.hpp", r"value = (\d+)", 5, 0, 10, 1)
        with self.assertRaises(spsa_search.TuningError):
            spsa_search.apply_values("value = 5\nvalue = 6\n", parameter, 7)


class UpdateTest(unittest.TestCase):
    def test_positive_gradient_raises_values(self):
        parameters = [
            spsa_search.Parameter("first", "x.hpp", r"(\d+)", 10, 0, 100, 2),
            spsa_search.Parameter("second", "x.hpp", r"(\d+)", 10, 0, 100, 2),
        ]
        updated = spsa_search.spsa_step(
            {"first": 10, "second": 10}, parameters,
            {"first": 1, "second": -1}, 0.60, 0.40, coefficient=50.0)
        self.assertEqual(updated["first"], 15)
        self.assertEqual(updated["second"], 5)

    def test_update_clamps_to_the_declared_range(self):
        parameters = [
            spsa_search.Parameter("first", "x.hpp", r"(\d+)", 99, 0, 100, 2),
        ]
        updated = spsa_search.spsa_step(
            {"first": 99}, parameters, {"first": 1}, 1.0, 0.0, coefficient=100.0)
        self.assertEqual(updated["first"], 100)


class DryRunTest(unittest.TestCase):
    def test_dry_run_stages_one_iteration_without_a_runner(self):
        with tempfile.TemporaryDirectory() as directory:
            staging = Path(directory) / "staging"
            report = Path(directory) / "report.json"
            result = spsa_search.run(
                root=ROOT,
                parameters=list(spsa_search.DEFAULT_PARAMETERS),
                staging_root=staging,
                runner="",
                iterations=1,
                games=10,
                seed=1,
                a0=2.0,
                alpha=0.602,
                a_bar=10.0,
                report_path=report,
                dry_run=True,
            )
            self.assertTrue(result["iterations"][0]["dry_run"])
            self.assertTrue((staging / "iter000-plus").is_dir())
            self.assertTrue((staging / "iter000-minus").is_dir())
            self.assertTrue(report.is_file())


if __name__ == "__main__":
    unittest.main()
