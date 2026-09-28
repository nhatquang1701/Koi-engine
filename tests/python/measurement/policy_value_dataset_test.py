import copy
import importlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))


def sample_record():
    return {
        "schema": "koi-policy-value-dataset-v1",
        "action_encoding": "koi-uci-action-v1",
        "position": {
            "fen": "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            "variant": "standard",
        },
        "legal_actions": [
            {"uci": "e2e4", "from": "e2", "to": "e4", "promotion": None},
            {"uci": "d2d4", "from": "d2", "to": "d4", "promotion": None},
        ],
        "policy_targets": [0.75, 0.25],
        "visit_counts": [3, 1],
        "value_target": 0.5,
        "outcome": "win",
        "opening_id": "opening-0001",
        "game_id": "selfplay-seed-7-game-1",
        "seed": 7,
        "ply": 0,
        "feature_schema": "koi-policy-features-v1",
        "producer": {
            "kind": "self-play",
            "name": "koi",
            "version": "0.9.0-dev",
        },
        "search_provenance": {
            "algorithm": "MCTS",
            "options": {"simulations": 64, "temperature": 1.0},
        },
        "network_sha256": "0123456789abcdef" * 4,
        "termination_reason": "checkmate",
    }


class PolicyValueDatasetTests(unittest.TestCase):
    def setUp(self):
        try:
            self.dataset = importlib.import_module("tools.measurement.policy_value_dataset")
        except ModuleNotFoundError as error:
            if error.name != "tools.measurement.policy_value_dataset":
                raise
            self.fail("policy_value_dataset module has not been implemented")

    def test_jsonl_round_trip_preserves_all_record_fields_and_action_order(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "samples.jsonl"
            self.dataset.write_jsonl(path, [sample_record()])
            loaded = list(self.dataset.read_jsonl(path))

        self.assertEqual(loaded, [sample_record()])
        self.assertEqual([item["uci"] for item in loaded[0]["legal_actions"]], ["e2e4", "d2d4"])

    def test_writer_creates_missing_parent_directories(self):
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "artifacts" / "training" / "samples.jsonl"
            self.assertFalse(path.parent.exists())

            self.dataset.write_jsonl(path, [sample_record()])

            self.assertTrue(path.parent.is_dir())
            self.assertEqual(list(self.dataset.read_jsonl(path)), [sample_record()])

    def test_jsonl_output_is_canonical_and_has_no_generated_metadata(self):
        record = sample_record()
        with tempfile.TemporaryDirectory() as temporary_directory:
            first = Path(temporary_directory) / "first.jsonl"
            second = Path(temporary_directory) / "second.jsonl"
            self.dataset.write_jsonl(first, [record])
            self.dataset.write_jsonl(second, [copy.deepcopy(record)])
            output = first.read_bytes()
            second_output = second.read_bytes()

        self.assertEqual(output, second_output)
        self.assertTrue(output.endswith(b"\n"))
        self.assertNotIn(b"timestamp", output)
        self.assertEqual(json.loads(output), record)

    def test_alpha_beta_distillation_may_omit_network_hash(self):
        record = sample_record()
        record["producer"]["kind"] = "alpha-beta-distillation"
        record["search_provenance"]["algorithm"] = "AlphaBeta"
        record["network_sha256"] = None

        self.dataset.validate_record(record)

    def test_self_play_requires_a_sha256_network_identity(self):
        record = sample_record()
        record["network_sha256"] = None

        with self.assertRaises(self.dataset.DatasetError):
            self.dataset.validate_record(record)

    def test_rejects_wrong_schema_and_action_encoding(self):
        for field, value in (("schema", "koi-policy-value-dataset-v2"),
                             ("action_encoding", "uci")):
            record = sample_record()
            record[field] = value
            with self.subTest(field=field), self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_rejects_action_target_and_visit_count_length_mismatches(self):
        mutations = (
            lambda record: record["policy_targets"].pop(),
            lambda record: record["visit_counts"].pop(),
        )
        for mutate in mutations:
            record = sample_record()
            mutate(record)
            with self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_visit_counts_must_match_the_normalized_policy_targets(self):
        invalid_pairs = (
            ([3, 1], [0.5, 0.5]),
            ([0, 0], [0.75, 0.25]),
            ([2, 2], [0.75, 0.25]),
        )
        for counts, targets in invalid_pairs:
            record = sample_record()
            record["visit_counts"] = counts
            record["policy_targets"] = targets
            with self.subTest(counts=counts, targets=targets), self.assertRaises(
                self.dataset.DatasetError
            ):
                self.dataset.validate_record(record)

    def test_rejects_invalid_uci_fields_and_duplicate_legal_actions(self):
        bad_actions = (
            [{"uci": "e2e5", "from": "e2", "to": "e4", "promotion": None},
             sample_record()["legal_actions"][1]],
            [{"uci": "a7a8x", "from": "a7", "to": "a8", "promotion": "x"},
             sample_record()["legal_actions"][1]],
            [sample_record()["legal_actions"][0], sample_record()["legal_actions"][0]],
        )
        for actions in bad_actions:
            record = sample_record()
            record["legal_actions"] = actions
            with self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_rejects_non_normalized_policy_and_invalid_value_or_outcome(self):
        invalid_records = []
        for targets in ([0.7, 0.2], [-0.1, 1.1], [float("nan"), 0.0]):
            record = sample_record()
            record["policy_targets"] = targets
            invalid_records.append(record)
        for field, value in (("value_target", 1.01), ("value_target", float("inf")),
                             ("value_target", 10**400), ("outcome", "white-win"),
                             ("outcome", [])):
            record = sample_record()
            record[field] = value
            invalid_records.append(record)

        for record in invalid_records:
            with self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_rejects_bad_hash_seed_and_negative_ply(self):
        for field, value in (("network_sha256", "not-a-sha256"), ("seed", -1), ("ply", -1)):
            record = sample_record()
            record[field] = value
            with self.subTest(field=field), self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_reader_rejects_malformed_truncated_and_duplicate_key_records(self):
        invalid_lines = (
            b'{"schema":"koi-policy-value-dataset-v1"\n',
            b'{"schema":"koi-policy-value-dataset-v1","schema":"duplicate"}\n',
        )
        for line in invalid_lines:
            with tempfile.TemporaryDirectory() as temporary_directory:
                path = Path(temporary_directory) / "broken.jsonl"
                path.write_bytes(line)
                with self.assertRaises(self.dataset.DatasetError):
                    list(self.dataset.read_jsonl(path))

    def test_reader_wraps_invalid_utf8_with_line_context(self):
        first_line = json.dumps(sample_record(), separators=(",", ":")).encode("utf-8")
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "invalid-utf8.jsonl"
            path.write_bytes(first_line + b"\n\xff\n")
            rows = self.dataset.read_jsonl(path)
            self.assertEqual(next(rows), sample_record())
            with self.assertRaisesRegex(self.dataset.DatasetError, "line 2"):
                next(rows)

    def test_reader_wraps_oversized_fen_counter_conversion_with_line_context(self):
        record = sample_record()
        record["position"]["fen"] = (
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - "
            + "9" * 5000
            + " 1"
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "oversized-fen.jsonl"
            path.write_text(json.dumps(record) + "\n", encoding="utf-8")
            with self.assertRaisesRegex(self.dataset.DatasetError, "line 1"):
                list(self.dataset.read_jsonl(path))

    def test_direct_validation_rejects_oversized_fen_counters_as_dataset_errors(self):
        for field_index in (4, 5):
            record = sample_record()
            fen_fields = record["position"]["fen"].split()
            fen_fields[field_index] = "9" * 5000
            record["position"]["fen"] = " ".join(fen_fields)
            with self.subTest(fen_field=field_index), self.assertRaises(self.dataset.DatasetError):
                self.dataset.validate_record(record)

    def test_reader_wraps_oversized_json_integer_with_line_context(self):
        encoded = json.dumps(sample_record()).replace('"seed": 7', '"seed": ' + "9" * 5000)
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "oversized-number.jsonl"
            path.write_text(encoded + "\n", encoding="utf-8")
            with self.assertRaisesRegex(self.dataset.DatasetError, "line 1"):
                list(self.dataset.read_jsonl(path))

    def test_writer_cleans_up_when_record_iterator_raises(self):
        def interrupted_records():
            yield sample_record()
            raise RuntimeError("producer failed")

        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "dataset.jsonl"
            path.write_bytes(b"previous contents\n")
            with self.assertRaisesRegex(RuntimeError, "producer failed"):
                self.dataset.write_jsonl(path, interrupted_records())

            self.assertEqual(path.read_bytes(), b"previous contents\n")
            self.assertEqual(list(Path(temporary_directory).glob(".dataset.jsonl.*.tmp")), [])

    def test_writer_preserves_existing_destination_when_a_later_record_is_invalid(self):
        invalid = sample_record()
        invalid["policy_targets"] = [0.4, 0.4]
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "dataset.jsonl"
            path.write_bytes(b"previous contents\n")
            with self.assertRaises(self.dataset.DatasetError):
                self.dataset.write_jsonl(path, [sample_record(), invalid])

            self.assertEqual(path.read_bytes(), b"previous contents\n")
            self.assertEqual(list(Path(temporary_directory).glob(".dataset.jsonl.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
