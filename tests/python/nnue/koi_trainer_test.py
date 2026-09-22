"""Cross-language tests for the KOI-NNUE version 4 trainer.

The tests cover the pieces the C++ engine and the Python trainer must agree on:

* the ``halfka-king-bucket-v1`` sparse encoder (pinned against the C++ fixture),
* the integer forward reference (hidden clipping, pair products, bucket head,
  arithmetic output shift) against C++ integer scores,
* the ``koi-dataset-v1`` binary loader,
* a tiny end-to-end export with metadata schema ``koi-nnue-training-metadata-v2``,
* byte-deterministic training and ``--float-in`` quantize-only reuse.

The cross-language cases run only when ``KOI_NNUE_BOUNDARY_EXE`` points at a
built ``nnue_boundary_tests`` executable (CMake wires it in CI and local runs).
The PyTorch cases are skipped when torch is not importable.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[3]
TOOLS_MEASUREMENT = REPO_ROOT / "tools" / "measurement"
TRAINER = TOOLS_MEASUREMENT / "train_nnue_koi.py"

sys.path.insert(0, str(TOOLS_MEASUREMENT))

import numpy as np  # noqa: E402

try:
    import koi_dataset  # noqa: E402
except ImportError:  # pragma: no cover - reported by the tests that need it
    koi_dataset = None

try:
    import train_nnue_koi  # noqa: E402
except ImportError:  # pragma: no cover - numpy is a hard trainer dependency
    train_nnue_koi = None

try:
    import torch  # noqa: F401

    TORCH_AVAILABLE = True
except Exception:  # pragma: no cover - optional dependency
    TORCH_AVAILABLE = False

import koi_chess as chess  # noqa: E402

HEADER = struct.Struct("<8sIIIIIBB2xHHQ")
HEADER_V5 = struct.Struct("<8sIIIIIBBBBHHQ")
HIDDEN_UNITS = 32
OUTPUT_BUCKETS = 8
QUANTIZATION = b"int16/int8"
FEATURE_SET = b"halfka-king-bucket-v1"


def parse_v4_container(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    (
        magic,
        version,
        input_units,
        hidden_units,
        output_buckets,
        reserved,
        hidden_shift,
        output_shift,
        quantization_length,
        feature_length,
        payload_length,
    ) = HEADER.unpack_from(data, 0)
    payload_hash = data[44:76].hex()
    quantization = data[76 : 76 + quantization_length]
    feature_set = data[
        76 + quantization_length : 76 + quantization_length + feature_length
    ]
    payload = data[76 + quantization_length + feature_length :]
    hidden_bias_bytes = hidden_units * 4
    output_weight_bytes = output_buckets * (hidden_units // 2)
    output_bias_bytes = output_buckets * 4
    feature_bytes = input_units * hidden_units * 2
    assert len(payload) == payload_length
    split = feature_bytes
    feature_weights = np.frombuffer(payload[:split], dtype="<i2").reshape(
        input_units, hidden_units
    )
    split += hidden_bias_bytes
    hidden_bias = np.frombuffer(payload[feature_bytes:split], dtype="<i4")
    output_weights = np.frombuffer(
        payload[split : split + output_weight_bytes], dtype="<i1"
    ).reshape(output_buckets, hidden_units // 2)
    split += output_weight_bytes
    output_bias = np.frombuffer(payload[split : split + output_bias_bytes], dtype="<i4")
    return {
        "magic": magic,
        "version": version,
        "input_units": input_units,
        "hidden_units": hidden_units,
        "output_buckets": output_buckets,
        "reserved": reserved,
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
        "quantization": quantization,
        "feature_set": feature_set,
        "payload_length": payload_length,
        "payload_hash": payload_hash,
        "network_hash": hashlib.sha256(data).hexdigest(),
        "feature_weights": feature_weights,
        "hidden_bias": hidden_bias,
        "output_weights": output_weights,
        "output_bias": output_bias,
        "params": {
            "feature_weights": feature_weights,
            "hidden_bias": hidden_bias,
            "output_weights": output_weights,
            "output_bias": output_bias,
            "hidden_shift": hidden_shift,
            "output_shift": output_shift,
        },
    }


def parse_v5_container(path: pathlib.Path) -> dict:
    data = path.read_bytes()
    (
        magic,
        version,
        input_units,
        hidden_units,
        output_buckets,
        l1_units,
        hidden_shift,
        output_shift,
        l1_shift,
        reserved,
        quantization_length,
        feature_length,
        payload_length,
    ) = HEADER_V5.unpack_from(data, 0)
    payload_hash = data[44:76].hex()
    quantization = data[76 : 76 + quantization_length]
    feature_set = data[
        76 + quantization_length : 76 + quantization_length + feature_length
    ]
    payload = data[76 + quantization_length + feature_length :]
    feature_bytes = input_units * hidden_units * 2
    hidden_bias_bytes = hidden_units * 4
    l1_weight_bytes = l1_units * hidden_units
    l1_bias_bytes = l1_units * 4
    output_weight_bytes = output_buckets * l1_units
    output_bias_bytes = output_buckets * 4
    assert len(payload) == payload_length
    split = feature_bytes
    feature_weights = np.frombuffer(payload[:split], dtype="<i2").reshape(
        input_units, hidden_units
    )
    split += hidden_bias_bytes
    hidden_bias = np.frombuffer(payload[feature_bytes:split], dtype="<i4")
    l1_weights = np.frombuffer(
        payload[split : split + l1_weight_bytes], dtype="<i1"
    ).reshape(l1_units, hidden_units)
    split += l1_weight_bytes
    l1_bias = np.frombuffer(payload[split : split + l1_bias_bytes], dtype="<i4")
    split += l1_bias_bytes
    output_weights = np.frombuffer(
        payload[split : split + output_weight_bytes], dtype="<i1"
    ).reshape(output_buckets, l1_units)
    split += output_weight_bytes
    output_bias = np.frombuffer(payload[split : split + output_bias_bytes], dtype="<i4")
    return {
        "magic": magic,
        "version": version,
        "input_units": input_units,
        "hidden_units": hidden_units,
        "output_buckets": output_buckets,
        "l1_units": l1_units,
        "reserved": reserved,
        "hidden_shift": hidden_shift,
        "output_shift": output_shift,
        "l1_shift": l1_shift,
        "quantization": quantization,
        "feature_set": feature_set,
        "payload_length": payload_length,
        "payload_hash": payload_hash,
        "network_hash": hashlib.sha256(data).hexdigest(),
        "feature_weights": feature_weights,
        "hidden_bias": hidden_bias,
        "l1_weights": l1_weights,
        "l1_bias": l1_bias,
        "output_weights": output_weights,
        "output_bias": output_bias,
        "params": {
            "feature_weights": feature_weights,
            "hidden_bias": hidden_bias,
            "l1_weights": l1_weights,
            "l1_bias": l1_bias,
            "output_weights": output_weights,
            "output_bias": output_bias,
            "hidden_shift": hidden_shift,
            "l1_shift": l1_shift,
            "output_shift": output_shift,
        },
    }


class EncoderParityTests(unittest.TestCase):
    def test_python_encoder_matches_cpp_fixture(self):
        executable = os.environ.get("KOI_NNUE_BOUNDARY_EXE")
        if not executable or not pathlib.Path(executable).is_file():
            self.skipTest("KOI_NNUE_BOUNDARY_EXE is not set to a built boundary test")
        self.assertIsNotNone(koi_dataset, "koi_dataset is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            container_path = pathlib.Path(directory) / "fixture.nnue"
            completed = subprocess.run(
                [executable, "--emit-v4-fixture", str(container_path)],
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(container_path.is_file())
            fixture = json.loads(completed.stdout.strip().splitlines()[-1])
            container = parse_v4_container(container_path)
            self.assertEqual(container["magic"], b"KOI-NNUE")
            self.assertEqual(container["version"], 4)
            self.assertEqual(container["hidden_units"], fixture["hidden_units"])
            self.assertEqual(container["hidden_shift"], fixture["hidden_shift"])
            self.assertEqual(container["output_shift"], fixture["output_shift"])
            for position in fixture["positions"]:
                board = chess.Board(position["fen"])
                indices = koi_dataset.halfka_king_bucket_indices(board)
                self.assertEqual(indices, position["indices"], position["fen"])
                hidden = train_nnue_koi.hidden_activations(
                    container["params"],
                    np.asarray(indices, dtype=np.int32),
                    np.asarray([0, len(indices)], dtype=np.int64),
                    np.asarray([0], dtype=np.int64),
                    container["hidden_units"],
                )
                buckets = np.asarray([koi_dataset.output_bucket(board)], dtype=np.int64)
                score = train_nnue_koi.integer_scores(
                    container["params"], hidden, buckets, container["hidden_units"]
                )
                self.assertEqual(int(score[0]), int(position["score"]))

    def test_python_encoder_matches_cpp_v5_fixture(self):
        executable = os.environ.get("KOI_NNUE_BOUNDARY_EXE")
        if not executable or not pathlib.Path(executable).is_file():
            self.skipTest("KOI_NNUE_BOUNDARY_EXE is not set to a built boundary test")
        self.assertIsNotNone(koi_dataset, "koi_dataset is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            container_path = pathlib.Path(directory) / "fixture-v5.nnue"
            completed = subprocess.run(
                [executable, "--emit-v5-fixture", str(container_path)],
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(container_path.is_file())
            fixture = json.loads(completed.stdout.strip().splitlines()[-1])
            container = parse_v5_container(container_path)
            self.assertEqual(container["magic"], b"KOI-NNUE")
            self.assertEqual(container["version"], 5)
            self.assertEqual(container["input_units"], 36864)
            self.assertEqual(container["hidden_units"], fixture["hidden_units"])
            self.assertEqual(container["l1_units"], fixture["l1_units"])
            self.assertEqual(container["hidden_shift"], fixture["hidden_shift"])
            self.assertEqual(container["l1_shift"], fixture["l1_shift"])
            self.assertEqual(container["output_shift"], fixture["output_shift"])
            for position in fixture["positions"]:
                board = chess.Board(position["fen"])
                a_stm, b_stm, a_opp, b_opp = koi_dataset.encode_record(board)
                self.assertEqual(
                    list(a_stm) + list(b_stm), position["own"], position["fen"]
                )
                self.assertEqual(
                    list(a_opp) + list(b_opp), position["opp"], position["fen"]
                )
                own = np.asarray(position["own"], dtype=np.int32)
                opp = np.asarray(position["opp"], dtype=np.int32)
                own_hidden, opp_hidden = train_nnue_koi.hidden_activations_dual(
                    container["params"],
                    own,
                    np.asarray([0, len(own)], dtype=np.int64),
                    opp,
                    np.asarray([0, len(opp)], dtype=np.int64),
                    np.asarray([0], dtype=np.int64),
                    container["hidden_units"],
                )
                buckets = np.asarray([koi_dataset.output_bucket(board)], dtype=np.int64)
                score = train_nnue_koi.integer_scores_v5(
                    container["params"], own_hidden, opp_hidden, buckets
                )
                self.assertEqual(int(score[0]), int(position["score"]))

    def test_integer_reference_clips_and_shifts(self):
        self.assertIsNotNone(train_nnue_koi)
        hidden_units = HIDDEN_UNITS
        params = {
            "feature_weights": np.zeros((train_nnue_koi.INPUT_UNITS, hidden_units), dtype=np.int64),
            "hidden_bias": np.zeros(hidden_units, dtype=np.int64),
            "output_weights": np.zeros((OUTPUT_BUCKETS, hidden_units // 2), dtype=np.int64),
            "output_bias": np.zeros(OUTPUT_BUCKETS, dtype=np.int64),
            "hidden_shift": 7,
            "output_shift": 4,
        }
        # Two features: index 0 pushes hidden 0 to 100, index 1 pushes hidden 16 to 50.
        params["feature_weights"][0, 0] = 100
        params["feature_weights"][1, hidden_units // 2] = 50
        indices = np.asarray([0, 1], dtype=np.int32)
        offsets = np.asarray([0, 2], dtype=np.int64)
        samples = np.asarray([0], dtype=np.int64)
        hidden = train_nnue_koi.hidden_activations(params, indices, offsets, samples, hidden_units)
        self.assertEqual(hidden.shape, (1, hidden_units))
        self.assertEqual(int(hidden[0, 0]), 100)
        self.assertEqual(int(hidden[0, hidden_units // 2]), 50)
        # Pair product 100 * 50 = 5000 with weight 100 and bias 7, shifted by 4.
        params["output_weights"][0, 0] = 100
        params["output_bias"][0] = 7
        buckets = np.asarray([0], dtype=np.int64)
        score = train_nnue_koi.integer_scores(params, hidden, buckets, hidden_units)
        self.assertEqual(int(score[0]), (7 + 100 * 5000) >> 4)
        # Extreme sums clamp to int32 first, then to the CReLU range.
        params["feature_weights"][0, 0] = train_nnue_koi.W1_LIMIT
        params["hidden_bias"][1] = train_nnue_koi.INT32_MAX
        params["feature_weights"][1, 1] = train_nnue_koi.W1_LIMIT
        hidden = train_nnue_koi.hidden_activations(
            params, np.asarray([0], dtype=np.int32), np.asarray([0, 1], dtype=np.int64),
            np.asarray([0], dtype=np.int64), hidden_units,
        )
        self.assertEqual(int(hidden[0, 0]), 127)
        self.assertEqual(int(hidden[0, 1]), 127)


class DatasetLoaderTests(unittest.TestCase):
    def test_binary_dataset_round_trip_and_rejections(self):
        self.assertIsNotNone(train_nnue_koi)
        records = [
            ([3, 8, 9], 12),
            ([], -7),
            ([9000, 9215], 0),
        ]
        blob = bytearray()
        blob += struct.pack("<8sIH", b"KOI-DATA", 1, len(FEATURE_SET))
        blob += FEATURE_SET
        blob += struct.pack("<Q", len(records))
        for indices, score in records:
            blob += struct.pack("<H", len(indices))
            blob += struct.pack(f"<{len(indices)}H", *indices)
            blob += struct.pack("<i", score)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "toy.koi-data"
            path.write_bytes(bytes(blob))
            indices, offsets, scores = train_nnue_koi.load_binary_dataset(path, 0)
            self.assertEqual(list(offsets), [0, 3, 3, 5])
            self.assertEqual(list(indices), [3, 8, 9, 9000, 9215])
            self.assertEqual(list(scores), [12, -7, 0])
            limited, offsets, scores = train_nnue_koi.load_binary_dataset(path, 2)
            self.assertEqual(list(limited), [3, 8, 9])
            self.assertEqual(list(offsets), [0, 3, 3])
            self.assertEqual(list(scores), [12, -7])
            with self.assertRaises(train_nnue_koi.TrainerError):
                train_nnue_koi.load_binary_dataset(path.parent / "missing", 0)
            bad_magic = pathlib.Path(directory) / "bad-magic.koi-data"
            bad_magic.write_bytes(b"KOI-DATX" + bytes(blob[8:]))
            with self.assertRaises(train_nnue_koi.TrainerError):
                train_nnue_koi.load_binary_dataset(bad_magic, 0)
            trailing = pathlib.Path(directory) / "trailing.koi-data"
            trailing.write_bytes(bytes(blob) + b"\x00\x00")
            with self.assertRaises(train_nnue_koi.TrainerError):
                train_nnue_koi.load_binary_dataset(trailing, 0)

    def test_binary_dataset_v2_buckets_follow_piece_counts(self):
        self.assertIsNotNone(train_nnue_koi)
        own_a_full = list(range(32))
        # 65 threat features push the combined A+B count past the old
        # feature-count heuristic, which derived a negative bucket and
        # raised IndexError on the output head.
        own_b_threats = [9216 + index for index in range(65)]
        records = [
            (own_a_full, [9216, 9217], own_a_full, [], 10),
            (list(range(4)), own_b_threats, list(range(3)), [], -5),
        ]
        blob = bytearray()
        blob += struct.pack("<8sIH", b"KOI-DATA", 2, 2)
        for group in (FEATURE_SET, b"threat-pairs-v1"):
            blob += struct.pack("<H", len(group))
            blob += group
        blob += struct.pack("<Q", len(records))
        for a_own, b_own, a_opp, b_opp, score in records:
            blob += struct.pack("<4H", len(a_own), len(b_own), len(a_opp), len(b_opp))
            for block in (a_own, b_own, a_opp, b_opp):
                blob += struct.pack(f"<{len(block)}H", *block)
            blob += struct.pack("<i", score)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "buckets.koi-data"
            path.write_bytes(bytes(blob))
            (own, own_offsets, opp, opp_offsets, scores,
             buckets) = train_nnue_koi.load_binary_dataset_v2(path, 0)
            self.assertEqual(list(scores), [10, -5])
            self.assertEqual(list(buckets), [0, 7])
            self.assertEqual(int(own_offsets[-1]), 32 + 2 + 4 + 65)
            self.assertEqual(int(opp_offsets[-1]), 32 + 0 + 3 + 0)

    def test_text_corpus_v5_buckets_follow_piece_counts(self):
        self.assertIsNotNone(train_nnue_koi)
        corpus = (
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1;20;e2e4\n"
            "8/8/8/4k3/8/8/4P3/4K3 w - - 0 1;90;e1d2\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "corpus.txt"
            path.write_text(corpus, encoding="utf-8")
            (own, own_offsets, opp, opp_offsets, scores,
             buckets) = train_nnue_koi.load_text_corpus_v5(path, 0)
            self.assertEqual(list(scores), [20, 90])
            self.assertEqual(list(buckets), [0, 7])


TRAINER_CORPUS = """\
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1;20;e2e4
rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1;-15;e7e5
rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2;35;g1f3
r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3;25;f1b5
r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4;-40;g8f6
8/8/8/4k3/8/8/4P3/4K3 w - - 0 1;90;e1d2
8/8/8/4k3/8/8/4P3/4K3 b - - 0 1;-90;e5d4
"""


@unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is not installed")
class TrainerPipelineTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.directory = pathlib.Path(self._temporary.name)
        self.corpus = self.directory / "toy-corpus.txt"
        rows = (TRAINER_CORPUS * ((64 // 7) + 1)).splitlines()[:64]
        self.corpus.write_text("\n".join(rows) + "\n", encoding="utf-8")

    def tearDown(self):
        self._temporary.cleanup()

    def run_trainer(self, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(TRAINER),
                "--corpus",
                str(self.corpus),
                "--rows",
                "64",
                "--epochs",
                "1",
                "--hidden-units",
                str(HIDDEN_UNITS),
                "--batch-size",
                "16",
                "--threads",
                "1",
                "--arch",
                "v4",
                "--hidden-shifts",
                "7",
                "--output-shifts",
                "12",
                *extra,
            ],
            cwd=str(REPO_ROOT),
            capture_output=True,
            text=True,
            timeout=600,
            check=False,
        )

    def test_export_metadata_and_determinism(self):
        first_net = self.directory / "first.nnue"
        first_meta = self.directory / "first.metadata.json"
        completed = self.run_trainer(
            "--net-out",
            str(first_net),
            "--meta-out",
            str(first_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("selected v4", completed.stdout)
        self.assertIn("wrote", completed.stdout)
        metadata = json.loads(first_meta.read_text(encoding="utf-8"))
        self.assertEqual(metadata["schema"], "koi-nnue-training-metadata-v2")
        self.assertEqual(metadata["feature_set"], "halfka-king-bucket-v1")
        self.assertEqual(metadata["quantization"], "int16/int8")
        self.assertEqual(
            metadata["architecture"],
            {"input": 9216, "hidden": HIDDEN_UNITS, "output_buckets": OUTPUT_BUCKETS},
        )
        self.assertEqual(metadata["activation"], "crelu-pair")
        self.assertEqual(metadata["rows_used"], 64)
        self.assertEqual(metadata["epochs"], 1)
        self.assertEqual(metadata["hidden_shift"], 7)
        self.assertEqual(metadata["output_shift"], 12)
        self.assertIn("val_mae_cp", metadata)
        self.assertIn("val_round_trip_mae_cp", metadata)
        self.assertIn("w1_saturation", metadata)
        self.assertIn("w2_saturation", metadata)
        self.assertIn("payload_sha256", metadata)
        self.assertIn("network_sha256", metadata)
        container = parse_v4_container(first_net)
        self.assertEqual(container["magic"], b"KOI-NNUE")
        self.assertEqual(container["version"], 4)
        self.assertEqual(container["input_units"], 9216)
        self.assertEqual(container["hidden_units"], HIDDEN_UNITS)
        self.assertEqual(container["output_buckets"], OUTPUT_BUCKETS)
        self.assertEqual(container["reserved"], 0)
        self.assertEqual(container["quantization"], QUANTIZATION)
        self.assertEqual(container["feature_set"], FEATURE_SET)
        expected_payload = (
            9216 * HIDDEN_UNITS * 2
            + HIDDEN_UNITS * 4
            + OUTPUT_BUCKETS * (HIDDEN_UNITS // 2)
            + OUTPUT_BUCKETS * 4
        )
        self.assertEqual(container["payload_length"], expected_payload)
        self.assertEqual(container["payload_hash"], metadata["payload_sha256"])
        self.assertEqual(container["network_hash"], metadata["network_sha256"])
        self.assertEqual(hashlib.sha256(first_net.read_bytes()).hexdigest(),
                         metadata["network_sha256"])

        second_net = self.directory / "second.nnue"
        second_meta = self.directory / "second.metadata.json"
        completed = self.run_trainer(
            "--net-out",
            str(second_net),
            "--meta-out",
            str(second_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(first_net.read_bytes(), second_net.read_bytes())

    def test_float_in_reuses_the_checkpoint(self):
        first_net = self.directory / "trained.nnue"
        first_meta = self.directory / "trained.metadata.json"
        checkpoint = self.directory / "trained.pt"
        completed = self.run_trainer(
            "--net-out",
            str(first_net),
            "--meta-out",
            str(first_meta),
            "--float-out",
            str(checkpoint),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(checkpoint.is_file())
        requantized_net = self.directory / "requantized.nnue"
        requantized_meta = self.directory / "requantized.metadata.json"
        completed = self.run_trainer(
            "--net-out",
            str(requantized_net),
            "--meta-out",
            str(requantized_meta),
            "--float-in",
            str(checkpoint),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("skipping training", completed.stdout)
        metadata = json.loads(requantized_meta.read_text(encoding="utf-8"))
        self.assertEqual(metadata["epochs"], 0)
        self.assertEqual(metadata["hidden_shift"], 7)
        self.assertEqual(metadata["output_shift"], 12)
        self.assertEqual(first_net.read_bytes(), requantized_net.read_bytes())

    def test_v5_export_metadata_and_determinism(self):
        first_net = self.directory / "first-v5.nnue"
        first_meta = self.directory / "first-v5.metadata.json"
        completed = self.run_trainer(
            "--arch",
            "v5",
            "--l1-units",
            "8",
            "--l1-shifts",
            "6",
            "--net-out",
            str(first_net),
            "--meta-out",
            str(first_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("selected v5", completed.stdout)
        metadata = json.loads(first_meta.read_text(encoding="utf-8"))
        self.assertEqual(metadata["schema"], "koi-nnue-training-metadata-v3")
        self.assertEqual(metadata["feature_set"], "halfka-king-bucket-v1+threat-pairs-v1")
        self.assertEqual(
            metadata["architecture"],
            {
                "input": 36864,
                "hidden": HIDDEN_UNITS,
                "l1": 8,
                "output_buckets": OUTPUT_BUCKETS,
                "groups": ["halfka-king-bucket-v1", "threat-pairs-v1"],
            },
        )
        self.assertEqual(metadata["activation"], "crelu-pair-l1")
        self.assertEqual(metadata["l1_shift"], 6)
        container = parse_v5_container(first_net)
        self.assertEqual(container["version"], 5)
        self.assertEqual(container["input_units"], 36864)
        self.assertEqual(container["l1_units"], 8)
        expected_payload = (
            36864 * HIDDEN_UNITS * 2
            + HIDDEN_UNITS * 4
            + 8 * HIDDEN_UNITS
            + 8 * 4
            + OUTPUT_BUCKETS * 8
            + OUTPUT_BUCKETS * 4
        )
        self.assertEqual(container["payload_length"], expected_payload)
        self.assertEqual(container["payload_hash"], metadata["payload_sha256"])
        self.assertEqual(container["network_hash"], metadata["network_sha256"])
        self.assertEqual(hashlib.sha256(first_net.read_bytes()).hexdigest(),
                         metadata["network_sha256"])

        second_net = self.directory / "second-v5.nnue"
        second_meta = self.directory / "second-v5.metadata.json"
        completed = self.run_trainer(
            "--arch",
            "v5",
            "--l1-units",
            "8",
            "--l1-shifts",
            "6",
            "--net-out",
            str(second_net),
            "--meta-out",
            str(second_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(first_net.read_bytes(), second_net.read_bytes())


if __name__ == "__main__":  # pragma: no cover - manual runs
    unittest.main()
