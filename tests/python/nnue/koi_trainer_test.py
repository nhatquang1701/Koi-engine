"""Cross-language tests for the KOI-NNUE version 4/5/6 trainer.

The tests cover the pieces the C++ engine and the Python trainer must agree on:

* the ``halfka-king-bucket-v1`` sparse encoder (pinned against the C++ fixture),
* the integer forward reference (hidden clipping, pair products, bucket head,
  arithmetic output shift) against C++ integer scores,
* the ``koi-dataset-v1`` binary loader,
* a tiny end-to-end export with metadata schema ``koi-nnue-training-metadata-v2``,
* byte-deterministic training and ``--float-in`` quantize-only reuse,
* the v6 antisymmetric diff head: integer round trip, container payload order,
  and the raw perspective direction/antisymmetry the pair-only v5 head lacks.

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


def parse_v6_container(path: pathlib.Path) -> dict:
    """Parse the version 6 container (v5 header plus the diff weight block).

    The header byte v5 keeps reserved carries ``l1_diff_shift`` for v6: a
    nonzero value selects the split head
    ``(pair_sum >> l1_shift) + (diff_sum >> l1_diff_shift)`` and 0 keeps the
    legacy combined ``(pair_sum + diff_sum) >> l1_shift`` interpretation.
    """
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
    l1_diff_shift = reserved
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
    feature_weights = np.frombuffer(
        payload[:feature_bytes], dtype="<i2"
    ).reshape(input_units, hidden_units)
    split = feature_bytes
    hidden_bias = np.frombuffer(
        payload[split : split + hidden_bias_bytes], dtype="<i4"
    )
    split += hidden_bias_bytes
    l1_weights = np.frombuffer(
        payload[split : split + l1_weight_bytes], dtype="<i1"
    ).reshape(l1_units, hidden_units)
    split += l1_weight_bytes
    l1_diff_weights = np.frombuffer(
        payload[split : split + l1_weight_bytes], dtype="<i1"
    ).reshape(l1_units, hidden_units)
    split += l1_weight_bytes
    l1_bias = np.frombuffer(
        payload[split : split + l1_bias_bytes], dtype="<i4"
    )
    split += l1_bias_bytes
    output_weights = np.frombuffer(
        payload[split : split + output_weight_bytes], dtype="<i1"
    ).reshape(output_buckets, l1_units)
    split += output_weight_bytes
    output_bias = np.frombuffer(
        payload[split : split + output_bias_bytes], dtype="<i4"
    )
    return {
        "magic": magic,
        "version": version,
        "input_units": input_units,
        "hidden_units": hidden_units,
        "output_buckets": output_buckets,
        "l1_units": l1_units,
        "reserved": reserved,
        "l1_diff_shift": l1_diff_shift,
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
        "l1_diff_weights": l1_diff_weights,
        "l1_bias": l1_bias,
        "output_weights": output_weights,
        "output_bias": output_bias,
        "params": {
            "feature_weights": feature_weights,
            "hidden_bias": hidden_bias,
            "l1_weights": l1_weights,
            "l1_diff_weights": l1_diff_weights,
            "l1_bias": l1_bias,
            "output_weights": output_weights,
            "output_bias": output_bias,
            "hidden_shift": hidden_shift,
            "l1_shift": l1_shift,
            "l1_diff_shift": l1_diff_shift,
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

    def test_python_encoder_matches_cpp_v6_fixture(self):
        executable = os.environ.get("KOI_NNUE_BOUNDARY_EXE")
        if not executable or not pathlib.Path(executable).is_file():
            self.skipTest("KOI_NNUE_BOUNDARY_EXE is not set to a built boundary test")
        self.assertIsNotNone(koi_dataset, "koi_dataset is unavailable")
        with tempfile.TemporaryDirectory() as directory:
            container_path = pathlib.Path(directory) / "fixture-v6.nnue"
            completed = subprocess.run(
                [executable, "--emit-v6-fixture", str(container_path)],
                capture_output=True,
                text=True,
                timeout=120,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertTrue(container_path.is_file())
            fixture = json.loads(completed.stdout.strip().splitlines()[-1])
            container = parse_v6_container(container_path)
            self.assertEqual(container["magic"], b"KOI-NNUE")
            self.assertEqual(container["version"], 6)
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
                score = train_nnue_koi.integer_scores_v6(
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


# ---------------------------------------------------------------------------
# v6 antisymmetric head helpers
# ---------------------------------------------------------------------------

V6_MATERIAL_VALUES = {
    chess.PAWN: 100,
    chess.KNIGHT: 320,
    chess.BISHOP: 330,
    chess.ROOK: 500,
    chess.QUEEN: 900,
}
# Kings on e1/e8 plus one piece on d1 or f3, both turns.  The queen-on-d1
# rows double as the trained-net evaluation pair below; the rest give the
# material target enough variety that the diff channel has to carry it.
V6_MATERIAL_SQUARES = ((0, 3), (2, 5))
V6_QUEEN_PAIR = (
    "4k3/8/8/8/8/8/8/3QK3 w - - 0 1",
    "4k3/8/8/8/8/8/8/3QK3 b - - 0 1",
)


def _board_fen(placement: dict) -> str:
    rows = []
    for rank_index in range(7, -1, -1):
        parts = []
        empty = 0
        for file_index in range(8):
            symbol = placement.get(rank_index * 8 + file_index)
            if symbol is None:
                empty += 1
                continue
            if empty:
                parts.append(str(empty))
                empty = 0
            parts.append(symbol)
        if empty:
            parts.append(str(empty))
        rows.append("".join(parts))
    return "/".join(rows)


def v6_material_rows() -> list[tuple[str, int]]:
    """``(fen, side-to-move cp)`` rows for the tiny v6 material corpus."""
    kings = {chess.square(4, 0): "K", chess.square(4, 7): "k"}
    rows: list[tuple[str, int]] = []
    for rank_index, file_index in V6_MATERIAL_SQUARES:
        for piece_type, value in V6_MATERIAL_VALUES.items():
            for color in (chess.WHITE, chess.BLACK):
                symbol = chess.PIECE_SYMBOLS[piece_type]
                if color == chess.BLACK:
                    symbol = symbol.lower()
                placement = dict(kings)
                placement[chess.square(file_index, rank_index)] = symbol
                board_fen = _board_fen(placement)
                white_cp = value if color == chess.WHITE else -value
                rows.append((f"{board_fen} w - - 0 1", white_cp))
                rows.append((f"{board_fen} b - - 0 1", -white_cp))
    board_fen = _board_fen(kings)
    rows.append((f"{board_fen} w - - 0 1", 0))
    rows.append((f"{board_fen} b - - 0 1", 0))
    return rows


def v6_encode_rows(rows) -> tuple[np.ndarray, ...]:
    own_rows, opp_rows, scores, buckets = [], [], [], []
    for fen, cp in rows:
        board = chess.Board(fen)
        a_own, b_own, a_opp, b_opp = koi_dataset.encode_record(board)
        own_rows.append(list(a_own) + list(b_own))
        opp_rows.append(list(a_opp) + list(b_opp))
        scores.append(cp)
        buckets.append(koi_dataset.output_bucket(board))
    own_flat, own_offsets, packed_scores = train_nnue_koi._pack_rows(own_rows, scores)
    opp_flat, opp_offsets, _ = train_nnue_koi._pack_rows(opp_rows, scores)
    return (own_flat, own_offsets, opp_flat, opp_offsets, packed_scores,
            np.asarray(buckets, dtype=np.int64))


def v6_train_material_model(steps: int = 500, hidden_units: int = 64,
                            l1_units: int = 32, seed: int = 20261006):
    """Train a tiny v6 head on the synthetic side-to-move material target.

    The small positive feature init keeps hidden activations visible without
    the saturation the production init needs many more steps to escape; the
    point of the test is the head architecture, not the training recipe.
    """
    torch.manual_seed(seed)
    (own_flat, own_offsets, opp_flat, opp_offsets, scores,
     buckets) = v6_encode_rows(v6_material_rows())
    rows = own_offsets.size - 1
    targets = scores.astype(np.float32) / train_nnue_koi.TARGET_SCALE
    pad_length = int(np.max(own_offsets[1:] - own_offsets[:-1]))
    model = train_nnue_koi.KoiNetV6(hidden_units, l1_units)
    with torch.no_grad():
        model.feature.weight.uniform_(0.0, 0.3)
        model.l1_weight.uniform_(-0.1, 0.1)
        model.l1_diff_weight.uniform_(-0.1, 0.1)
        model.output_weight.uniform_(-1.0, 1.0)
        model.hidden_bias.zero_()
        model.l1_bias.zero_()
        model.output_bias.zero_()
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-2, weight_decay=1e-4)
    loss_fn = torch.nn.MSELoss(reduction="sum")
    order = np.arange(rows)
    for _ in range(steps):
        model.train()
        for (own, own_weights, opp, opp_weights, batch_buckets,
             batch_targets) in train_nnue_koi.batches_dual(
                own_flat, own_offsets, opp_flat, opp_offsets, buckets, targets,
                order, rows, pad_length):
            optimizer.zero_grad()
            prediction = model(own, own_weights, opp, opp_weights, batch_buckets)
            loss = loss_fn(prediction, batch_targets)
            loss.backward()
            optimizer.step()
    model.eval()
    return model


def v6_raw_cp(model, fen: str) -> float:
    """Raw (unquantized) side-to-move score of one FEN, in target units."""
    board = chess.Board(fen)
    a_own, b_own, a_opp, b_opp = koi_dataset.encode_record(board)
    own = np.asarray(list(a_own) + list(b_own), dtype=np.int64)
    opp = np.asarray(list(a_opp) + list(b_opp), dtype=np.int64)
    own_flat = torch.from_numpy(own[None, :])
    opp_flat = torch.from_numpy(opp[None, :])
    weights = torch.ones_like(own_flat, dtype=torch.float32)
    buckets = torch.from_numpy(
        np.asarray([koi_dataset.output_bucket(board)], dtype=np.int64))
    with torch.no_grad():
        output = model(own_flat, weights, opp_flat, weights, buckets)
    return float(output[0]) * train_nnue_koi.TARGET_SCALE


@unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is not installed")
class V6HeadTests(unittest.TestCase):
    """Integer, container, and trained raw checks for the v6 diff head."""

    @classmethod
    def setUpClass(cls):
        cls.model = v6_train_material_model()

    def test_integer_reference_round_trip(self):
        self.assertIsNotNone(train_nnue_koi)
        torch.manual_seed(11)
        model = train_nnue_koi.KoiNetV6(64, 8)
        with torch.no_grad():
            for name, parameter in model.named_parameters():
                if name == "output_weight":
                    parameter.uniform_(-0.1, 0.1)
                else:
                    parameter.copy_(torch.randn_like(parameter) * 0.3)
        hidden_shift, l1_shift, l1_diff_shift, output_shift = 7, 9, 8, 6
        params = train_nnue_koi.quantize_v6(model, hidden_shift, l1_shift,
                                            l1_diff_shift)
        params["output_weights"] = np.clip(
            np.rint(model.output_weight.detach().numpy().astype(np.float64)
                    * train_nnue_koi.TARGET_SCALE * 2.0**output_shift / 127.0),
            -train_nnue_koi.L1_LIMIT, train_nnue_koi.L1_LIMIT).astype(np.int64)
        params["output_bias"] = np.rint(
            model.output_bias.detach().numpy().astype(np.float64)
            * train_nnue_koi.TARGET_SCALE * 2.0**output_shift).astype(np.int64)
        params["output_shift"] = output_shift
        rng = np.random.default_rng(12)
        own = rng.integers(0, 128, size=(24, 64)).astype(np.int64)
        opp = rng.integers(0, 128, size=(24, 64)).astype(np.int64)
        buckets = rng.integers(0, 8, size=24)
        scores = train_nnue_koi.integer_scores_v6(params, own, opp, buckets)
        # Dequantized reference in the same integer L1 units; the two channels
        # shift on their own scales and floor, so a sub-unit difference is
        # expected.
        pairs = own * opp
        diff = own - opp
        l1 = (pairs @ params["l1_weights"].T.astype(np.float64) / 2.0**l1_shift
              + diff @ params["l1_diff_weights"].T.astype(np.float64) / 2.0**l1_diff_shift
              + params["l1_bias"].astype(np.float64))
        l1 = np.clip(l1, 0.0, 127.0)
        reference = (
            params["output_bias"][buckets].astype(np.float64) / 2.0**output_shift
            + (l1 * params["output_weights"][buckets].astype(np.float64)
               / 2.0**output_shift).sum(axis=1))
        error = np.abs(scores.astype(np.float64) - reference)
        self.assertLess(float(error.mean()), 1.0)
        self.assertLess(float(error.max()), 2.0)

    def test_container_payload_layout(self):
        self.assertIsNotNone(train_nnue_koi)
        torch.manual_seed(13)
        model = train_nnue_koi.KoiNetV6(64, 8)
        params = train_nnue_koi.quantize_v6(model, 7, 8, 9)
        params["output_weights"] = np.clip(
            np.rint(model.output_weight.detach().numpy().astype(np.float64)
                    * train_nnue_koi.TARGET_SCALE * 2.0**6 / 127.0),
            -train_nnue_koi.L1_LIMIT, train_nnue_koi.L1_LIMIT).astype(np.int64)
        params["output_bias"] = np.rint(
            model.output_bias.detach().numpy().astype(np.float64)
            * train_nnue_koi.TARGET_SCALE * 2.0**6).astype(np.int64)
        params["output_shift"] = 6
        payload = train_nnue_koi.nnue_payload_v6(params, 64, 8)
        container, payload_hash = train_nnue_koi.nnue_container_v6(
            payload, 64, 8, 7, 6, 8, 9)
        header = struct.unpack_from("<8sIIIIIBBBBHHQ", container, 0)
        self.assertEqual(header[0], b"KOI-NNUE")
        self.assertEqual(header[1], 6)
        self.assertEqual(header[2], 36864)
        self.assertEqual(header[3], 64)
        self.assertEqual(header[4], OUTPUT_BUCKETS)
        self.assertEqual(header[5], 8)
        # The 4th scale byte was reserved in v5 and now carries the split
        # pair/diff head's diff shift.
        self.assertEqual(header[6:10], (7, 6, 8, 9))
        self.assertEqual(header[10], len(QUANTIZATION))
        self.assertEqual(header[11], len(FEATURE_SET + b"+threat-pairs-v1"))
        self.assertEqual(header[12], len(payload))
        feature_bytes = 36864 * 64 * 2
        hidden_bias_bytes = 64 * 4
        l1_bytes = 8 * 64
        l1_bias_bytes = 8 * 4
        output_bytes = OUTPUT_BUCKETS * 8
        output_bias_bytes = OUTPUT_BUCKETS * 4
        self.assertEqual(len(payload), feature_bytes + hidden_bias_bytes
                         + 2 * l1_bytes + l1_bias_bytes + output_bytes
                         + output_bias_bytes)
        # The int8 diff weights directly follow the int8 pair weights.
        split = feature_bytes + hidden_bias_bytes + l1_bytes
        diff = np.frombuffer(payload[split : split + l1_bytes], dtype="<i1")
        self.assertEqual(list(diff),
                         list(params["l1_diff_weights"].astype("<i1").reshape(-1)))
        self.assertEqual(payload_hash, hashlib.sha256(payload).hexdigest())

    def test_raw_perspective_direction(self):
        # Same board, white to move versus black to move.  The v5 pair-only
        # head returns the same raw value for both; the v6 diff channel must
        # move the side-to-move score by more than the test tolerance.
        white = v6_raw_cp(self.model, V6_QUEEN_PAIR[0])
        black = v6_raw_cp(self.model, V6_QUEEN_PAIR[1])
        self.assertGreater(white - black, 5.0)
        self.assertGreater(white, 0.0)
        self.assertLess(black, 0.0)

    def test_raw_antisymmetry(self):
        # Swapping the own/opponent perspectives must approximately negate the
        # raw score; the trained tiny net fits both turns of the queen pair.
        white = v6_raw_cp(self.model, V6_QUEEN_PAIR[0])
        black = v6_raw_cp(self.model, V6_QUEEN_PAIR[1])
        self.assertLess(abs(white + black), 5.0)


class V6ContainerByteTests(unittest.TestCase):
    """v6 header byte semantics: nonzero selects the split pair/diff head."""

    @staticmethod
    def _params(l1_shift: int, l1_diff_shift: int) -> dict:
        rng = np.random.default_rng(20261008)
        hidden_units = HIDDEN_UNITS
        l1_units = 8
        return {
            "feature_weights": rng.integers(
                -1000, 1000, size=(train_nnue_koi.V5_INPUT_UNITS, hidden_units),
                dtype=np.int64),
            "hidden_bias": rng.integers(
                -100, 100, size=hidden_units, dtype=np.int64),
            "l1_weights": rng.integers(
                -127, 128, size=(l1_units, hidden_units), dtype=np.int64),
            "l1_diff_weights": rng.integers(
                -127, 128, size=(l1_units, hidden_units), dtype=np.int64),
            "l1_bias": rng.integers(-500, 500, size=l1_units, dtype=np.int64),
            "output_weights": rng.integers(
                -127, 128, size=(OUTPUT_BUCKETS, l1_units), dtype=np.int64),
            "output_bias": rng.integers(
                -500, 500, size=OUTPUT_BUCKETS, dtype=np.int64),
            "hidden_shift": 7,
            "l1_shift": l1_shift,
            "l1_diff_shift": l1_diff_shift,
            "output_shift": 6,
        }

    @staticmethod
    def _expected_scores(params: dict, own, opp, buckets, split: bool):
        """Reference integer scores for one of the two head interpretations."""
        pairs = own * opp
        diff = own - opp
        pair_sum = pairs @ params["l1_weights"].T
        diff_sum = diff @ params["l1_diff_weights"].T
        if split:
            pre = ((pair_sum >> params["l1_shift"])
                   + (diff_sum >> params["l1_diff_shift"])
                   + params["l1_bias"])
        else:
            pre = (pair_sum + diff_sum + params["l1_bias"]) >> params["l1_shift"]
        l1 = np.clip(pre, 0, 127)
        output = (params["output_bias"][buckets]
                  + (l1 * params["output_weights"][buckets]).sum(axis=1))
        return output >> params["output_shift"]

    @staticmethod
    def _positions():
        own = np.random.default_rng(21).integers(
            0, 128, size=(6, HIDDEN_UNITS)).astype(np.int64)
        opp = np.random.default_rng(22).integers(
            0, 128, size=(6, HIDDEN_UNITS)).astype(np.int64)
        buckets = np.arange(6, dtype=np.int64) % OUTPUT_BUCKETS
        return own, opp, buckets

    def test_v6_container_diff_shift_round_trip(self):
        self.assertIsNotNone(train_nnue_koi)
        params = self._params(l1_shift=8, l1_diff_shift=9)
        payload = train_nnue_koi.nnue_payload_v6(params, HIDDEN_UNITS, 8)
        container, payload_hash = train_nnue_koi.nnue_container_v6(
            payload, HIDDEN_UNITS, 8, 7, 6, 8, 9)
        # The formerly reserved header byte at offset 31 now carries the diff
        # shift; a nonzero value selects the split pair/diff head.
        self.assertEqual(container[31], 9)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "split-v6.nnue"
            path.write_bytes(container)
            parsed = parse_v6_container(path)
            self.assertEqual(parsed["version"], 6)
            self.assertEqual(parsed["hidden_shift"], 7)
            self.assertEqual(parsed["l1_shift"], 8)
            self.assertEqual(parsed["l1_diff_shift"], 9)
            self.assertEqual(parsed["reserved"], 9)
            self.assertEqual(parsed["payload_hash"], payload_hash)
            self.assertTrue(np.array_equal(
                parsed["l1_diff_weights"],
                params["l1_diff_weights"].astype("<i1")))
            own, opp, buckets = self._positions()
            scores = train_nnue_koi.integer_scores_v6(
                parsed["params"], own, opp, buckets)
            self.assertTrue(np.array_equal(
                scores,
                self._expected_scores(
                    parsed["params"], own, opp, buckets, split=True)))

    def test_v6_reserved_zero_keeps_legacy_combined_shift(self):
        self.assertIsNotNone(train_nnue_koi)
        params = self._params(l1_shift=8, l1_diff_shift=0)
        payload = train_nnue_koi.nnue_payload_v6(params, HIDDEN_UNITS, 8)
        container, _ = train_nnue_koi.nnue_container_v6(
            payload, HIDDEN_UNITS, 8, 7, 6, 8, 0)
        self.assertEqual(container[31], 0)
        own, opp, buckets = self._positions()
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "legacy-v6.nnue"
            path.write_bytes(container)
            parsed = parse_v6_container(path)
            self.assertEqual(parsed["l1_diff_shift"], 0)
            self.assertEqual(parsed["reserved"], 0)
            legacy = train_nnue_koi.integer_scores_v6(
                parsed["params"], own, opp, buckets)
            self.assertTrue(np.array_equal(
                legacy,
                self._expected_scores(
                    parsed["params"], own, opp, buckets, split=False)))
            # The same weights with a nonzero byte must take the split head, so
            # the two interpretations are observably different.
            split_params = dict(parsed["params"], l1_diff_shift=9)
            split = train_nnue_koi.integer_scores_v6(split_params, own, opp, buckets)
            self.assertFalse(np.array_equal(legacy, split))


@unittest.skipUnless(TORCH_AVAILABLE, "PyTorch is not installed")
class V5BlindnessTests(unittest.TestCase):
    def test_v5_raw_is_invariant_under_the_perspective_swap(self):
        # Pins the defect the v6 diff channel fixes: the pair-only head cannot
        # tell whose favour the evaluation is, so raw(own, opp) == raw(opp, own).
        torch.manual_seed(1)
        model = train_nnue_koi.KoiNetV5(32, 8)
        with torch.no_grad():
            for parameter in model.parameters():
                parameter.copy_(torch.randn_like(parameter) * 0.2)
        own = torch.randint(0, 36864, (4, 16))
        opp = torch.randint(0, 36864, (4, 16))
        weights = torch.ones_like(own, dtype=torch.float32)
        buckets = torch.randint(0, OUTPUT_BUCKETS, (4,))
        direct = model(own, weights, opp, weights, buckets)
        swapped = model(opp, weights, own, weights, buckets)
        self.assertTrue(torch.equal(direct, swapped))


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

    def test_v6_export_metadata_and_determinism(self):
        first_net = self.directory / "first-v6.nnue"
        first_meta = self.directory / "first-v6.metadata.json"
        completed = self.run_trainer(
            "--arch",
            "v6",
            "--l1-units",
            "8",
            "--net-out",
            str(first_net),
            "--meta-out",
            str(first_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("selected v6", completed.stdout)
        metadata = json.loads(first_meta.read_text(encoding="utf-8"))
        self.assertEqual(metadata["schema"], "koi-nnue-training-metadata-v3")
        self.assertEqual(metadata["arch"], "v6")
        self.assertEqual(metadata["activation"], "crelu-pair-diff-l1")
        self.assertEqual(metadata["feature_set"], "halfka-king-bucket-v1+threat-pairs-v1")
        # The export searches the pair (8..14) and diff (8..10) shifts
        # independently and keeps the historical 4..8 output range.
        self.assertGreaterEqual(metadata["l1_shift"], 8)
        self.assertLessEqual(metadata["l1_shift"], 14)
        self.assertGreaterEqual(metadata["l1_diff_shift"], 8)
        self.assertLessEqual(metadata["l1_diff_shift"], 10)
        self.assertGreaterEqual(metadata["output_shift"], 4)
        self.assertLessEqual(metadata["output_shift"], 8)
        self.assertEqual(metadata["best_epoch"], 1)
        self.assertEqual(metadata["epochs_run"], 1)
        self.assertIn("l1_pair_zero_fraction", metadata)
        self.assertIn("l1_saturation", metadata)
        self.assertIn("l1_diff_saturation", metadata)
        container = parse_v6_container(first_net)
        self.assertEqual(container["magic"], b"KOI-NNUE")
        self.assertEqual(container["version"], 6)
        self.assertEqual(container["input_units"], 36864)
        self.assertEqual(container["l1_units"], 8)
        self.assertEqual(container["l1_shift"], metadata["l1_shift"])
        self.assertEqual(container["l1_diff_shift"], metadata["l1_diff_shift"])
        self.assertEqual(container["output_shift"], metadata["output_shift"])
        expected_payload = (
            36864 * HIDDEN_UNITS * 2
            + HIDDEN_UNITS * 4
            + 2 * 8 * HIDDEN_UNITS
            + 8 * 4
            + OUTPUT_BUCKETS * 8
            + OUTPUT_BUCKETS * 4
        )
        self.assertEqual(container["payload_length"], expected_payload)
        self.assertEqual(container["payload_hash"], metadata["payload_sha256"])
        self.assertEqual(container["network_hash"], metadata["network_sha256"])
        self.assertEqual(hashlib.sha256(first_net.read_bytes()).hexdigest(),
                         metadata["network_sha256"])

        second_net = self.directory / "second-v6.nnue"
        second_meta = self.directory / "second-v6.metadata.json"
        completed = self.run_trainer(
            "--arch",
            "v6",
            "--l1-units",
            "8",
            "--net-out",
            str(second_net),
            "--meta-out",
            str(second_meta),
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(first_net.read_bytes(), second_net.read_bytes())


if __name__ == "__main__":  # pragma: no cover - manual runs
    unittest.main()
