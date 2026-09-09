import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "tune_eval.py"
ROWS = {
    "train": "4k3/8/8/8/8/8/P7/4K3 w - - 0 1,1-0\n",
    "validation": "4k3/8/8/8/8/8/1P6/4K3 w - - 0 1,1/2-1/2\n",
    "holdout": "4k3/8/8/8/8/8/2P5/4K3 w - - 0 1,0-1\n",
}


class NnueTrainingBoundaryTest(unittest.TestCase):
    def test_synthetic_export_is_deterministic_and_records_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            split_paths = {}
            for name, text in ROWS.items():
                path = directory / f"{name}.csv"
                path.write_text(text, encoding="utf-8")
                split_paths[name] = path

            network_a = directory / "a.nnue"
            metadata_a = directory / "a.json"
            network_b = directory / "b.nnue"
            metadata_b = directory / "b.json"
            command = [
                sys.executable,
                str(TOOL),
                "nnue",
                "--train",
                str(split_paths["train"]),
                "--validation",
                str(split_paths["validation"]),
                "--holdout",
                str(split_paths["holdout"]),
                "--output-network",
                str(network_a),
                "--output-metadata",
                str(metadata_a),
                "--seed",
                "17",
                "--backend",
                "synthetic",
                "--device",
                "cpu",
            ]
            first = subprocess.run(command, cwd=ROOT, text=True,
                                   capture_output=True, check=False)
            self.assertEqual(first.returncode, 0, first.stderr)

            second_command = command.copy()
            second_command[second_command.index(str(network_a))] = str(network_b)
            second_command[second_command.index(str(metadata_a))] = str(metadata_b)
            second = subprocess.run(second_command, cwd=ROOT, text=True,
                                    capture_output=True, check=False)
            self.assertEqual(second.returncode, 0, second.stderr)

            self.assertEqual(network_a.read_bytes(), network_b.read_bytes())
            network_bytes = network_a.read_bytes()
            self.assertEqual(network_bytes[:8], b"KOI-NNUE")
            manifest = struct.unpack("<8sI4IHHQ", network_bytes[:40])
            self.assertEqual(manifest[1:6], (2, 960, 256, 32, 1))
            payload_offset = 72 + manifest[6] + manifest[7]
            self.assertEqual(
                hashlib.sha256(network_bytes[payload_offset:]).digest(),
                network_bytes[40:72],
            )
            metadata = json.loads(metadata_a.read_text(encoding="utf-8"))
            self.assertEqual(metadata["schema"], "koi-nnue-training-metadata-v1")
            self.assertEqual(metadata["seed"], 17)
            self.assertEqual(metadata["architecture"], [960, 256, 32, 1])
            self.assertEqual(metadata["backend"], "synthetic")
            self.assertEqual(metadata["device"], "cpu")
            self.assertEqual(set(metadata["split_hashes"]), set(ROWS))
            self.assertEqual(metadata["split_counts"], {name: 1 for name in ROWS})
            self.assertEqual(
                metadata["network_sha256"],
                hashlib.sha256(network_bytes).hexdigest(),
            )
            self.assertEqual(metadata["command"][0], sys.executable)
            boundary_executable = os.environ.get("KOI_NNUE_BOUNDARY_EXE")
            if boundary_executable:
                boundary = subprocess.run(
                    [boundary_executable, str(network_a)],
                    cwd=ROOT,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(boundary.returncode, 0, boundary.stderr + boundary.stdout)

    def test_synthetic_export_is_small_corpus_and_gpu_independent(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            paths = {}
            for name, text in ROWS.items():
                paths[name] = directory / f"{name}.csv"
                paths[name].write_text(text, encoding="utf-8")
            output = directory / "network.nnue"
            metadata = directory / "network.json"
            result = subprocess.run(
                [sys.executable, str(TOOL), "nnue",
                 "--train", str(paths["train"]),
                 "--validation", str(paths["validation"]),
                 "--holdout", str(paths["holdout"]),
                 "--output-network", str(output),
                 "--output-metadata", str(metadata),
                 "--backend", "synthetic", "--device", "cpu", "--seed", "1"],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertLess(output.stat().st_size, 1_000_000)

    def test_manifest_input_resolves_relative_split_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            for name, text in ROWS.items():
                (directory / f"{name}.csv").write_text(text, encoding="utf-8")
            manifest = directory / "splits.json"
            manifest.write_text(
                json.dumps({"train": "train.csv", "validation": "validation.csv",
                            "holdout": "holdout.csv"}),
                encoding="utf-8",
            )
            output = directory / "manifest.nnue"
            result = subprocess.run(
                [sys.executable, str(TOOL), "nnue", "--manifest", str(manifest),
                 "--output-network", str(output), "--seed", "2"],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(output.exists())

    def test_torch_backend_reports_missing_optional_dependency(self):
        if importlib.util.find_spec("torch") is not None:
            self.skipTest("PyTorch is installed; dependency-missing branch is not applicable")
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            paths = {}
            for name, text in ROWS.items():
                paths[name] = directory / f"{name}.csv"
                paths[name].write_text(text, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(TOOL), "nnue",
                 "--train", str(paths["train"]),
                 "--validation", str(paths["validation"]),
                 "--holdout", str(paths["holdout"]),
                 "--output-network", str(directory / "network.nnue"),
                 "--backend", "torch", "--device", "cpu"],
                cwd=ROOT, text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("PyTorch is required", result.stderr)


if __name__ == "__main__":
    unittest.main()
