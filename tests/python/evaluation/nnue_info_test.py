"""Tests for the NNUE container inspector."""

import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
INFO_PATH = ROOT / "tools" / "measurement" / "nnue_info.py"
TRAINER_PATH = ROOT / "tools" / "measurement" / "train_nnue_koi.py"


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NnueInfoTest(unittest.TestCase):
    def setUp(self):
        self.info = _load(INFO_PATH, "nnue_info")
        self.trainer = _load(TRAINER_PATH, "train_nnue_koi_info")

    def test_v8_container_reports_gpu_ready(self):
        payload = b"\x01" * 64
        container, payload_hash = self.trainer.nnue_container_v6(
            payload, 32, 8, 7, 14, 7, 8, version=self.trainer.V8_VERSION)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "v8.nnue"
            path.write_bytes(container)
            info = self.info.read_container(path)
        self.assertEqual(info["version"], 8)
        self.assertEqual(info["hidden"], 32)
        self.assertEqual(info["l1"], 8)
        self.assertEqual(info["output_buckets"], 8)
        self.assertTrue(info["gpu_ready"])
        self.assertEqual(info["payload_sha256"], payload_hash)
        self.assertEqual(struct.unpack_from("<I", container, 8)[0], 8)

    def test_rejects_non_container_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "bad.nnue"
            path.write_bytes(b"not-a-network" * 4)
            with self.assertRaises(self.info.NnueInfoError):
                self.info.read_container(path)


if __name__ == "__main__":
    unittest.main()
