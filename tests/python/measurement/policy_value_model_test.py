import importlib
import math
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))


class PolicyValueModelTests(unittest.TestCase):
    def setUp(self):
        try:
            self.module = importlib.import_module("tools.measurement.policy_value_model")
        except ModuleNotFoundError as error:
            if error.name != "tools.measurement.policy_value_model":
                raise
            self.fail("policy_value_model module has not been implemented")

    def make_model(self):
        model = self.module.PolicyValueModel.zeros()
        model.state_embedding[0, 0] = 1.0
        model.state_embedding[1, 1] = 2.0
        model.state_bias[:] = 1.0
        model.from_embedding[0, :] = 0.1
        model.to_embedding[8, :] = 0.2
        model.promotion_embedding[1, :] = 0.3
        model.policy_bias = 0.25
        model.value_weights[0, :] = [1.0, 0.0, -1.0]
        model.value_bias[:] = [0.0, 0.0, 0.0]
        return model

    def actions(self):
        return [
            {"uci": "a1a2", "from": "a1", "to": "a2", "promotion": None},
            {"uci": "b1b2q", "from": "b1", "to": "b2", "promotion": "q"},
        ]

    def test_model_file_round_trip_preserves_weights_and_evaluation(self):
        model = self.make_model()
        expected = model.evaluate([0, 1], self.actions())
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            loaded = self.module.PolicyValueModel.read(path)
        actual = loaded.evaluate([0, 1], self.actions())
        self.assertEqual(actual.priors, expected.priors)
        self.assertEqual(actual.wdl, expected.wdl)
        self.assertEqual(actual.value, expected.value)
        for name in self.module.WEIGHT_NAMES:
            np.testing.assert_array_equal(getattr(loaded, name), getattr(model, name))

    def test_header_is_exact_v1_little_endian_contract(self):
        model = self.make_model()
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            encoded = path.read_bytes()
        header = struct.unpack("<8sIIIIIQI", encoded[:40])
        payload = encoded[40:]
        self.assertEqual(header, (
            b"KOIPV1\0\0", 1, 5, 1, 64, 36864, len(payload),
            zlib.crc32(payload) & 0xFFFFFFFF,
        ))
        self.assertEqual(len(encoded), 40 + header[6])
        self.assertEqual(header[6], self.module.PAYLOAD_BYTES)

    def test_payload_tensor_offsets_and_little_endian_values_are_fixed(self):
        model = self.module.PolicyValueModel.zeros()
        sentinels = {
            "state_embedding": 1.25,
            "state_bias": 2.5,
            "from_embedding": 3.75,
            "to_embedding": 4.5,
            "promotion_embedding": 5.25,
            "policy_bias": 0.1,
            "value_weights": 6.5,
            "value_bias": 7.75,
        }
        for name, value in sentinels.items():
            if name == "policy_bias":
                model.policy_bias = value
            else:
                getattr(model, name).flat[0] = value
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            encoded = path.read_bytes()
            loaded = self.module.PolicyValueModel.read(path)

        sizes = {
            "state_embedding": 36864 * 64 * 4,
            "state_bias": 64 * 4,
            "from_embedding": 64 * 64 * 4,
            "to_embedding": 64 * 64 * 4,
            "promotion_embedding": 5 * 64 * 4,
            "policy_bias": 4,
            "value_weights": 64 * 3 * 4,
            "value_bias": 3 * 4,
        }
        offset = 40
        for name, value in sentinels.items():
            with self.subTest(name=name):
                self.assertEqual(struct.unpack_from("<f", encoded, offset)[0], float(np.float32(value)))
                if name == "policy_bias":
                    self.assertEqual(loaded.policy_bias, float(np.float32(value)))
                else:
                    self.assertEqual(float(getattr(loaded, name).flat[0]), float(np.float32(value)))
            offset += sizes[name]
        self.assertEqual(offset, len(encoded))

    def test_policy_bias_is_float32_quantized_before_evaluation_and_round_trip(self):
        base = self.module.PolicyValueModel.zeros()
        model = self.module.PolicyValueModel(
            base.state_embedding, base.state_bias, base.from_embedding, base.to_embedding,
            base.promotion_embedding, 0.1, base.value_weights, base.value_bias,
        )
        self.assertEqual(model.policy_bias, float(np.float32(0.1)))
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            loaded = self.module.PolicyValueModel.read(path)
        self.assertEqual(model.policy_bias, loaded.policy_bias)
        self.assertEqual(model.evaluate([0], self.actions()), loaded.evaluate([0], self.actions()))

    def test_reassigned_policy_bias_stays_float32_quantized_through_round_trip(self):
        model = self.module.PolicyValueModel.zeros()
        model.policy_bias = 0.1
        self.assertEqual(model.policy_bias, float(np.float32(0.1)))
        before_write = model.evaluate([0], self.actions())
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            loaded = self.module.PolicyValueModel.read(path)
        self.assertEqual(before_write, loaded.evaluate([0], self.actions()))

    def test_write_creates_missing_parent_directories(self):
        model = self.make_model()
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "nested" / "models" / "model.kpv"
            model.write(path)
            loaded = self.module.PolicyValueModel.read(path)
        self.assertEqual(loaded.evaluate([0, 1], self.actions()), model.evaluate([0, 1], self.actions()))

    def test_rejects_bad_magic_version_schema_action_dimensions_crc_and_lengths(self):
        model = self.make_model()
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            original = path.read_bytes()
            header = list(struct.unpack("<8sIIIIIQI", original[:40]))
            cases = []
            for index, value in ((0, b"BADMAGIC"), (1, 2), (2, 6), (3, 2), (4, 63), (5, 36863)):
                changed = header.copy()
                changed[index] = value
                cases.append(("unsupported header", struct.pack("<8sIIIIIQI", *changed) + original[40:]))
            cases.extend((
                ("truncated header", original[:39]),
                ("truncated payload", original[:-1]),
                ("trailing bytes", original + b"x"),
                ("crc mismatch", original[:40] + bytes([original[40] ^ 1]) + original[41:]),
            ))
            for label, data in cases:
                with self.subTest(label=label):
                    path.write_bytes(data)
                    with self.assertRaises(self.module.PolicyValueModelError):
                        self.module.PolicyValueModel.read(path)

    def test_rejects_non_finite_weights(self):
        model = self.make_model()
        model.value_bias[1] = np.nan
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "nonfinite.kpv"
            with self.assertRaises(self.module.PolicyValueModelError):
                model.write(path)
            self.assertFalse(path.exists())

    def test_priors_follow_supplied_legal_action_order_and_only_legal_actions(self):
        model = self.make_model()
        actions = self.actions()
        result = model.evaluate([0], actions)
        reversed_result = model.evaluate([0], actions[::-1])
        self.assertAlmostEqual(sum(result.priors), 1.0)
        self.assertGreater(result.priors[1], result.priors[0])
        self.assertEqual(reversed_result.priors, result.priors[::-1])

    def test_wdl_softmax_is_stable_for_large_finite_logits(self):
        model = self.module.PolicyValueModel.zeros()
        model.value_bias[:] = [10000.0, 9999.0, -10000.0]
        result = model.evaluate([0], self.actions())
        self.assertTrue(all(math.isfinite(probability) for probability in result.wdl))
        self.assertAlmostEqual(sum(result.wdl), 1.0)
        self.assertAlmostEqual(result.value, result.wdl[0] - result.wdl[2])

    def test_black_positions_keep_canonical_sparse_features_and_mirror_actions(self):
        model = self.module.PolicyValueModel.zeros()
        model.state_embedding[0, 0] = 1.0
        model.state_embedding[56, 0] = 3.0
        model.value_weights[0, :] = [1.0, 0.0, -1.0]
        white_actions = [{"uci": "a1a2", "from": "a1", "to": "a2", "promotion": None}]
        black_actions = [{"uci": "a8a7", "from": "a8", "to": "a7", "promotion": None}]
        white = model.evaluate([0], white_actions, side_to_move="white")
        black = model.evaluate([0], black_actions, side_to_move="black")
        self.assertEqual(white.priors, black.priors)
        self.assertEqual(white.wdl, black.wdl)
        self.assertEqual(white.value, black.value)
    def test_rejects_invalid_sparse_indices_and_legal_actions(self):
        model = self.make_model()
        invalid_features = ([36864], [-1], [True], [0, 0], [1.5])
        for features in invalid_features:
            with self.subTest(features=features), self.assertRaises(self.module.PolicyValueModelError):
                model.evaluate(features, self.actions())
        invalid_actions = (
            [],
            [{"uci": "a1a2", "from": "a1", "to": "a2", "promotion": "q"}],
            [{"uci": "a1a1", "from": "a1", "to": "a1", "promotion": None}],
            [{"uci": "a1a2x", "from": "a1", "to": "a2", "promotion": "x"}],
            [{"uci": "a1a2", "from": "a1", "to": "a2", "promotion": None}] * 2,
        )
        for actions in invalid_actions:
            with self.subTest(actions=actions), self.assertRaises(self.module.PolicyValueModelError):
                model.evaluate([0], actions)

    def test_sha256_helper_hashes_model_bytes(self):
        model = self.make_model()
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "model.kpv"
            model.write(path)
            self.assertEqual(self.module.sha256_file(path), __import__("hashlib").sha256(path.read_bytes()).hexdigest())


if __name__ == "__main__":
    unittest.main()
