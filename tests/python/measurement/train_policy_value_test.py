import hashlib
import importlib
import json
import os
import platform
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools" / "measurement"))

import koi_chess as chess
import policy_value_dataset


class TrainPolicyValueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.trainer = importlib.import_module("tools.measurement.train_policy_value")
        cls.model_module = importlib.import_module("tools.measurement.policy_value_model")

    def records(self, prefix):
        rows = []
        for index, fen in enumerate((chess.STARTING_FEN, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1")):
            board = chess.Board(fen)
            actions = []
            legal_moves = list(board.legal_moves)
            for move in legal_moves:
                actions.append({
                    "uci": move.uci(), "from": chess.square_name(move.from_square),
                    "to": chess.square_name(move.to_square),
                    "promotion": chess.piece_symbol(move.promotion) if move.promotion else None,
                })
            rows.append({
                "schema": policy_value_dataset.SCHEMA,
                "action_encoding": policy_value_dataset.ACTION_ENCODING,
                "position": {"fen": fen, "variant": "standard"},
                "legal_actions": actions,
                "policy_targets": [1.0 / len(actions)] * len(actions),
                "value_target": 0.25 if index == 0 else -0.25,
                "outcome": "win" if index == 0 else "loss",
                "opening_id": f"{prefix}-opening-{index}",
                "game_id": f"{prefix}-game-{index}",
                "seed": 100 + index,
                "ply": index,
                "feature_schema": "halfka-threat-v5",
                "producer": {"kind": "self-play", "name": "test", "version": "1"},
                "search_provenance": {"algorithm": "MCTS", "options": {}},
                "network_sha256": "0" * 64,
                "termination_reason": "checkmate",
            })
        return rows

    def write_records(self, path, records):
        policy_value_dataset.write_jsonl(path, records)

    def test_rejects_game_or_opening_leakage_between_splits(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        with tempfile.TemporaryDirectory() as temporary:
            train_path = Path(temporary) / "train.jsonl"
            validation_path = Path(temporary) / "validation.jsonl"
            train = self.records("train")
            validation = self.records("validation")
            validation[0]["game_id"] = train[0]["game_id"]
            self.write_records(train_path, train)
            self.write_records(validation_path, validation)
            with self.assertRaisesRegex(ValueError, "game_id overlap"):
                self.trainer.train_policy_value(train_path, validation_path, Path(temporary) / "x.kpv", Path(temporary) / "x.json", epochs=1, device="cpu")

            validation[0]["game_id"] = "independent-game"
            validation[1]["opening_id"] = train[1]["opening_id"]
            self.write_records(validation_path, validation)
            with self.assertRaisesRegex(ValueError, "opening_id overlap"):
                self.trainer.train_policy_value(train_path, validation_path, Path(temporary) / "x.kpv", Path(temporary) / "x.json", epochs=1, device="cpu")

    def test_rejects_incomplete_legal_move_list(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        with tempfile.TemporaryDirectory() as temporary:
            train_path = Path(temporary) / "train.jsonl"
            validation_path = Path(temporary) / "validation.jsonl"
            train = self.records("train")
            train[0]["legal_actions"].pop()
            train[0]["policy_targets"] = [1.0 / len(train[0]["legal_actions"])] * len(train[0]["legal_actions"])
            self.write_records(train_path, train)
            self.write_records(validation_path, self.records("validation"))
            with self.assertRaisesRegex(ValueError, "complete legal move set"):
                self.trainer.train_policy_value(train_path, validation_path, Path(temporary) / "x.kpv", Path(temporary) / "x.json", epochs=1)

    def test_rejects_resolved_output_path_collisions_before_writing(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            train_path, validation_path = root / "train.jsonl", root / "validation.jsonl"
            self.write_records(train_path, self.records("train"))
            self.write_records(validation_path, self.records("validation"))
            original = train_path.read_bytes()
            with self.assertRaisesRegex(ValueError, "paths must resolve to distinct files"):
                self.trainer.train_policy_value(train_path, validation_path, root / "." / "train.jsonl", root / "meta.json", epochs=1)
            self.assertEqual(train_path.read_bytes(), original)

    def test_missing_torch_has_a_clear_training_error(self):
        with mock.patch.object(self.trainer, "torch", None):
            with self.assertRaisesRegex(RuntimeError, "PyTorch is required to train"):
                self.trainer._require_torch()

    def test_module_imports_when_torch_is_unavailable(self):
        script = r"""
import builtins, importlib, sys
sys.path.insert(0, sys.argv[1])
original_import = builtins.__import__
def without_torch(name, *args, **kwargs):
    if name == 'torch' or name.startswith('torch.'):
        raise ModuleNotFoundError("No module named 'torch'", name='torch')
    return original_import(name, *args, **kwargs)
builtins.__import__ = without_torch
trainer = importlib.import_module('tools.measurement.train_policy_value')
assert trainer.torch is None
try:
    trainer.train_policy_value('unused', 'unused', 'unused', 'unused')
except RuntimeError as error:
    assert 'PyTorch is required to train' in str(error)
else:
    raise AssertionError('training should require PyTorch')
"""
        completed = subprocess.run(
            [sys.executable, "-c", script, str(ROOT)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_metadata_replace_failure_preserves_previous_file_and_cleans_temp(self):
        with tempfile.TemporaryDirectory() as temporary:
            metadata_path = Path(temporary) / "metadata.json"
            original = b'{"previous":true}\n'
            metadata_path.write_bytes(original)
            with mock.patch.object(self.trainer.os, "replace", side_effect=OSError("replace failed")):
                with self.assertRaisesRegex(OSError, "replace failed"):
                    self.trainer._write_metadata_atomic(metadata_path, {"current": True})
            self.assertEqual(metadata_path.read_bytes(), original)
            self.assertEqual(list(Path(temporary).glob("*.tmp")), [])

    def test_cuda_sync_helper_calls_torch_synchronize_without_gpu_execution(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        cuda_device = mock.Mock(type="cuda")
        with mock.patch.object(self.trainer.torch.cuda, "synchronize") as synchronize:
            self.trainer._synchronize_device(cuda_device)
        synchronize.assert_called_once_with(cuda_device)

    def test_cuda_workspace_is_set_before_availability_check(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("CUBLAS_WORKSPACE_CONFIG", None)
            with mock.patch.object(self.trainer.torch.cuda, "is_initialized", return_value=False), \
                 mock.patch.object(self.trainer.torch.cuda, "is_available", side_effect=lambda: self.assertEqual(os.environ.get("CUBLAS_WORKSPACE_CONFIG"), ":4096:8") or False):
                with self.assertRaisesRegex(RuntimeError, "CUDA was requested but is unavailable"):
                    self.trainer.train_policy_value("unused", "unused", "unused", "unused", device="cuda")

    def test_cuda_workspace_rejects_initialized_cuda_with_incompatible_config(self):
        if self.trainer.torch is None:
            self.skipTest("PyTorch is not installed")
        with mock.patch.dict(os.environ, {"CUBLAS_WORKSPACE_CONFIG": ":invalid"}):
            with mock.patch.object(self.trainer.torch.cuda, "is_initialized", return_value=True):
                with self.assertRaisesRegex(RuntimeError, "CUDA is already initialized.*CUBLAS_WORKSPACE_CONFIG"):
                    self.trainer._configure_cuda_determinism()

    def test_cuda_provenance_has_build_and_bounded_driver_fields(self):
        completed = mock.Mock(stdout="555.42.02\n")
        with mock.patch.object(self.trainer.subprocess, "run", return_value=completed) as run:
            self.assertEqual(self.trainer._cuda_driver_version(), "555.42.02")
        self.assertEqual(run.call_args.kwargs["timeout"], 2.0)
        self.assertEqual(run.call_args.args[0][0], "nvidia-smi")
        with mock.patch.object(self.trainer, "_cuda_driver_version", return_value=None):
            metadata = self.trainer._cuda_provenance()
        self.assertIn("cuda_build_version", metadata)
        self.assertIn("cuda_driver_version", metadata)
        self.assertIsNone(metadata["cuda_driver_version"])

    def test_cpu_training_exports_reloadable_model_and_reproducible_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            train_path, validation_path = root / "train.jsonl", root / "validation.jsonl"
            self.write_records(train_path, self.records("train"))
            self.write_records(validation_path, self.records("validation"))
            outputs = []
            observed_sync_types = []
            sync_device = self.trainer._synchronize_device

            def observe_sync(device):
                observed_sync_types.append(device.type)
                sync_device(device)

            with mock.patch.object(self.trainer, "_synchronize_device", side_effect=observe_sync):
                for run in ("one", "two"):
                    model_path, metadata_path = root / f"{run}.kpv", root / f"{run}.json"
                    metrics = self.trainer.train_policy_value(
                        train_path, validation_path, model_path, metadata_path,
                        epochs=1, batch_size=2, seed=17,
                    )
                    model = self.model_module.PolicyValueModel.read(model_path)
                    board = chess.Board(chess.STARTING_FEN)
                    features = self.trainer.position_features(board)
                    evaluation = model.evaluate(features, self.records("eval")[0]["legal_actions"])
                    self.assertAlmostEqual(sum(evaluation.priors), 1.0, places=6)
                    self.assertEqual(len(evaluation.wdl), 3)
                    metadata = json.loads(metadata_path.read_bytes())
                    self.assertEqual(metadata["train_sha256"], hashlib.sha256(train_path.read_bytes()).hexdigest())
                    self.assertEqual(metadata["validation_sha256"], hashlib.sha256(validation_path.read_bytes()).hexdigest())
                    self.assertEqual(metadata["model_sha256"], hashlib.sha256(model_path.read_bytes()).hexdigest())
                    self.assertEqual(metadata["device"], "cpu")
                    self.assertEqual(metadata["metrics"], metrics)
                    counts = metadata["dataset_counts"]
                    self.assertEqual(counts, {"train": 2, "validation": 2})
                    timing = metadata["timing"]
                    self.assertGreater(timing["training_seconds"], 0.0)
                    self.assertGreater(timing["validation_seconds"], 0.0)
                    self.assertGreater(timing["train_positions_per_second"], 0.0)
                    self.assertGreater(timing["validation_positions_per_second"], 0.0)
                    self.assertEqual(timing["train_positions_per_second"], timing["training_positions"] / timing["training_seconds"])
                    self.assertEqual(timing["validation_positions_per_second"], 2 / timing["validation_seconds"])
                    environment = metadata["environment"]
                    self.assertEqual(environment["device"], "cpu")
                    self.assertTrue(environment["host"]["machine"])
                    self.assertTrue(environment["host"]["logical_cpu_count"])
                    self.assertTrue(environment["pytorch_version"])
                    self.assertTrue(environment["device_name"])
                    self.assertEqual(environment["python_version"], platform.python_version())
                    self.assertTrue(environment["numpy_version"])
                    self.assertIn("cuda_available", environment)
                    self.assertIn("cuda_devices", environment)
                    self.assertIn("cuda_build_version", environment)
                    self.assertIn("cuda_driver_version", environment)
                    self.assertIsInstance(metadata["source_dirty"], bool)
                    source_hashes = metadata["source_sha256"]
                    for relative_path in self.trainer.SOURCE_PATHS:
                        source_file = ROOT / Path(relative_path)
                        self.assertEqual(source_hashes[relative_path], hashlib.sha256(source_file.read_bytes()).hexdigest())
                    deterministic = {
                        key: metadata[key] for key in (
                            "schema", "train_sha256", "validation_sha256", "source_revision",
                            "source_dirty", "source_sha256", "model_sha256", "hyperparameters",
                            "device", "metrics",
                        )
                    }
                    outputs.append((model_path.read_bytes(), deterministic))
            self.assertEqual(observed_sync_types, ["cpu"] * 8)
            self.assertEqual(outputs[0], outputs[1])


if __name__ == "__main__":
    unittest.main()
