"""Tests for the bulletformat dataset converter (tools/nnue/to_bullet.py)."""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

try:
    import numpy as np  # noqa: F401

    BULLET_TOOLING_AVAILABLE = True
except ImportError:  # pragma: no cover - environment dependent
    np = None  # type: ignore[assignment]
    BULLET_TOOLING_AVAILABLE = False

REPO_ROOT = Path(__file__).resolve().parents[3]
NNUE_DIR = REPO_ROOT / "tools" / "nnue"
if str(NNUE_DIR) not in sys.path:
    sys.path.insert(0, str(NNUE_DIR))

try:
    import chess  # noqa: F401

    CHESS_AVAILABLE = True
except ImportError:  # pragma: no cover - environment dependent
    CHESS_AVAILABLE = False

try:
    import run_bullet  # noqa: E402  (also puts tools/measurement on sys.path)
    import export_bullet_v4  # noqa: E402
    import export_bullet_v5  # noqa: E402
    import to_bullet  # noqa: E402
except ImportError:  # pragma: no cover - environment dependent
    BULLET_TOOLING_AVAILABLE = False

STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
BLACK_TO_MOVE = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"


@unittest.skipUnless(BULLET_TOOLING_AVAILABLE, "numpy and the bullet tooling are required")
@unittest.skipUnless(CHESS_AVAILABLE, "python-chess is required for the converter")
class TextConversionTests(unittest.TestCase):
    def test_scores_are_converted_to_white_relative(self) -> None:
        self.assertEqual(to_bullet.bullet_text_row(STARTPOS, True, 42), f"{STARTPOS} | 42 | 0.5")
        self.assertEqual(to_bullet.bullet_text_row(BLACK_TO_MOVE, False, 42), f"{BLACK_TO_MOVE} | -42 | 0.5")
        self.assertEqual(to_bullet.bullet_text_row(BLACK_TO_MOVE, False, -17), f"{BLACK_TO_MOVE} | 17 | 0.5")

    def test_split_limit_and_malformed_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            corpus = directory / "labels.txt"
            lines = [f"{STARTPOS};{index};e2e4" for index in range(21)]
            lines.insert(5, "not-a-fen;42;e2e4")
            lines.insert(11, f"{STARTPOS};99999;e2e4")
            corpus.write_text("\n".join(lines) + "\n", encoding="utf-8")

            train_rows, validation_rows = to_bullet.convert_labels(
                corpus, directory / "out", val_fraction=0.05, log=lambda _: None
            )
            self.assertEqual(train_rows, 19)
            self.assertEqual(validation_rows, 2)  # indices 0 and 20
            train_text = (directory / "out" / "train.txt").read_text(encoding="utf-8").splitlines()
            validation_text = (
                (directory / "out" / "validation.txt").read_text(encoding="utf-8").splitlines()
            )
            self.assertEqual(len(train_text), 19)
            self.assertEqual(len(validation_text), 2)
            self.assertIn(" | 1 | 0.5", train_text[0])
            self.assertIn(" | 0 | 0.5", validation_text[0])

            train_rows, validation_rows = to_bullet.convert_labels(
                corpus, directory / "capped", val_fraction=0.05, limit=10, log=lambda _: None
            )
            self.assertEqual(train_rows + validation_rows, 10)
            self.assertEqual(validation_rows, 1)  # only index 0

    def test_val_fraction_zero_writes_no_validation_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            corpus = directory / "labels.txt"
            corpus.write_text(f"{STARTPOS};10;e2e4\n{BLACK_TO_MOVE};-10;e7e5\n", encoding="utf-8")
            train_rows, validation_rows = to_bullet.convert_labels(
                corpus, directory / "out", val_fraction=0.0, log=lambda _: None
            )
            self.assertEqual((train_rows, validation_rows), (2, 0))
            self.assertFalse((directory / "out" / "validation.txt").exists())

    def test_missing_input_raises(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(to_bullet.ConversionError):
                to_bullet.convert_labels(Path(temp) / "absent.txt", Path(temp) / "out")


@unittest.skipUnless(BULLET_TOOLING_AVAILABLE, "numpy and the bullet tooling are required")
@unittest.skipUnless(CHESS_AVAILABLE, "python-chess is required for the converter")
class CliTests(unittest.TestCase):
    def test_text_only_does_not_need_the_convert_binary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            corpus = directory / "labels.txt"
            corpus.write_text(f"{STARTPOS};10;e2e4\n", encoding="utf-8")
            code = to_bullet.main(
                [
                    "--input",
                    str(corpus),
                    "--output-dir",
                    str(directory / "out"),
                    "--text-only",
                ]
            )
            self.assertEqual(code, 0)
            self.assertTrue((directory / "out" / "train.txt").is_file())

    def test_binary_conversion_when_the_convert_binary_exists(self) -> None:
        convert_exe = to_bullet.find_convert_exe()
        if convert_exe is None:
            self.skipTest("bullet_train convert binary is not built")
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            corpus = directory / "labels.txt"
            corpus.write_text(
                f"{STARTPOS};10;e2e4\n{BLACK_TO_MOVE};-10;e7e5\n{STARTPOS};-5;d2d4\n",
                encoding="utf-8",
            )
            code = to_bullet.main(
                [
                    "--input",
                    str(corpus),
                    "--output-dir",
                    str(directory / "out"),
                    "--val-fraction",
                    "0",
                ]
            )
            self.assertEqual(code, 0)
            data = (directory / "out" / "train.data").read_bytes()
            self.assertEqual(len(data) % 32, 0)
            self.assertEqual(len(data) // 32, 3)
            occupancy = struct.unpack_from("<Q", data, 0)[0]
            self.assertNotEqual(occupancy, 0)


@unittest.skipUnless(BULLET_TOOLING_AVAILABLE, "numpy and the bullet tooling are required")
class RunBulletTests(unittest.TestCase):
    """Pure parsing and command building for the bullet wrapper."""

    def test_parse_bullet_line_handles_the_cursor_up_prefix(self) -> None:
        # bullet prints the superbatch summary after an \x1b[F cursor move, so
        # the stripped line begins with the estimated-time text.
        line = (
            "Estimated time to end of superbatch: 1.2s     "
            "superbatch 3 | time 2.5s | running loss 0.125 | 1000 pos/sec | total time 7.5s"
        )
        event = run_bullet.parse_bullet_line(line)
        self.assertIsNotNone(event)
        assert event is not None
        self.assertEqual(event["kind"], "superbatch")
        self.assertEqual(event["superbatch"], 3)
        self.assertAlmostEqual(event["seconds"], 2.5)
        self.assertAlmostEqual(event["loss"], 0.125)

    def test_parse_bullet_line_strips_ansi(self) -> None:
        event = run_bullet.parse_bullet_line(
            "\x1b[31msuperbatch\x1b[0m 1 | time 1.0s | running loss 0.5"
        )
        self.assertIsNotNone(event)
        assert event is not None
        self.assertAlmostEqual(event["loss"], 0.5)

    def test_parse_bullet_line_reads_saved_and_ignores_the_rest(self) -> None:
        self.assertEqual(run_bullet.parse_bullet_line("Saved [koi-v4-7]"),
                         {"kind": "saved", "name": "koi-v4-7"})
        self.assertIsNone(run_bullet.parse_bullet_line("Total Training Time: 0h 0m 1s"))

    def test_format_epoch_line_matches_the_studio_contract(self) -> None:
        self.assertEqual(
            run_bullet.format_epoch_line(2, 10, 0.25, 707.4, 1.5),
            "epoch 2/10 train_loss 0.25000 val_loss 0.25000 val_mae_cp 707.4 time 1.5s",
        )

    def test_command_builders(self) -> None:
        train = run_bullet.build_train_command(
            Path("bullet_train.exe"), Path("train.data"), Path("out"), "koi-v4",
            1024, 8192, 610, 100, 0.002, 0.0002, 20260916, 4, 10,
        )
        self.assertIn("--hidden", train)
        self.assertEqual(train[train.index("--hidden") + 1], "1024")
        self.assertEqual(train[train.index("--lr") + 1], repr(0.002))

        export = run_bullet.build_export_command(
            Path("checkpoints"), Path("validation.txt"), Path("koi.nnue"),
            Path("koi.metadata.json"), 1024, [6, 7, 8], [12, 14], 4000,
        )
        self.assertIn("export_bullet_v4.py", export[1])
        self.assertEqual(export[export.index("--hidden-shifts") + 1:export.index("--output-shifts")],
                         ["6", "7", "8"])
        self.assertEqual(export[-1], "4000")

        convert = run_bullet.build_convert_command(Path("labels.txt"), Path("bullet"), 0.05, 0)
        self.assertIn("to_bullet.py", convert[1])
        self.assertEqual(convert[convert.index("--val-fraction") + 1], repr(0.05))


@unittest.skipUnless(BULLET_TOOLING_AVAILABLE, "numpy and the bullet tooling are required")
class ExporterTests(unittest.TestCase):
    """Pins the bullet raw.bin layout read by export_bullet_v4."""

    def test_infer_hidden_units(self) -> None:
        hidden = 1024
        self.assertEqual(export_bullet_v4.infer_hidden_units(hidden * export_bullet_v4.BYTES_PER_HIDDEN + 32), hidden)
        with self.assertRaises(export_bullet_v4.ExportError):
            export_bullet_v4.infer_hidden_units(12345)

    def test_read_raw_weights_uses_the_feature_major_layout(self) -> None:
        hidden = 32
        half = hidden // 2
        feature_major = np.arange(export_bullet_v4.train_nnue_koi.INPUT_UNITS * hidden, dtype=np.float32)
        feature_major = feature_major.reshape(export_bullet_v4.train_nnue_koi.INPUT_UNITS, hidden) / 1000.0
        hidden_bias = np.linspace(-1.0, 1.0, hidden, dtype=np.float32)
        bucket_major = np.arange(8 * half, dtype=np.float32).reshape(8, half) / 100.0
        output_bias = np.linspace(-0.5, 0.5, 8, dtype=np.float32)

        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            # raw.bin stores each affine column-major; for (hidden, 9216) that
            # equals row-major (9216, hidden) = feature-major, and for (8, half)
            # it equals row-major (half, 8) which the reader transposes back.
            raw.write_bytes(
                feature_major.astype("<f4").tobytes()
                + hidden_bias.astype("<f4").tobytes()
                + bucket_major.T.astype("<f4").tobytes()
                + output_bias.astype("<f4").tobytes()
            )
            stored_hidden, weights = export_bullet_v4.read_raw_weights(raw)

        self.assertEqual(stored_hidden, hidden)
        np.testing.assert_allclose(weights["feature_weights"], feature_major, rtol=0, atol=1e-3)
        np.testing.assert_allclose(weights["hidden_bias"], hidden_bias, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["output_weights"], bucket_major, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["output_bias"], output_bias, rtol=0, atol=1e-6)
        self.assertEqual(weights["feature_weights"].shape, (9216, hidden))
        self.assertEqual(weights["output_weights"].shape, (8, half))

    def test_export_command_parses_with_exact_options(self) -> None:
        command = run_bullet.build_export_command(
            Path("checkpoints"),
            Path("validation.txt"),
            Path("net.nnue"),
            Path("net.metadata.json"),
            1024,
            [6, 7, 8],
            [12, 14, 16, 18, 20],
            4000,
        )
        option_strings = export_bullet_v4.build_parser()._option_string_actions
        for token in command[2:]:
            if token.startswith("--"):
                self.assertIn(token, option_strings, f"{token} is not an exact exporter option")
        args = export_bullet_v4.build_parser().parse_args(command[2:])
        self.assertEqual(args.hidden, 1024)
        self.assertEqual(args.hidden_shifts, [6, 7, 8])
        self.assertEqual(args.output_shifts, [12, 14, 16, 18, 20])
        self.assertEqual(args.tune_samples, 4000)

    def test_exporter_rejects_a_hidden_mismatch(self) -> None:
        hidden = 32
        half = hidden // 2
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            raw.write_bytes(
                np.zeros(export_bullet_v4.train_nnue_koi.INPUT_UNITS * hidden, dtype="<f4").tobytes()
                + np.zeros(hidden, dtype="<f4").tobytes()
                + np.zeros(8 * half, dtype="<f4").tobytes()
                + np.zeros(8, dtype="<f4").tobytes()
            )
            exit_code = export_bullet_v4.main([
                "--checkpoint", str(raw),
                "--hidden", "64",
                "--net-out", str(Path(temp) / "net.nnue"),
                "--meta-out", str(Path(temp) / "net.metadata.json"),
            ])
        self.assertEqual(exit_code, 2)

    def test_validation_rows_with_inf_or_nan_are_skipped(self) -> None:
        valid = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 | 17 | 0.5"
        midgame = "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4 | -31 | 0.5"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "validation.txt"
            path.write_text(
                "\n".join([
                    valid,
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 | inf | 0.5",
                    midgame,
                    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1 | nan | 0.5",
                    "",
                ]),
                encoding="utf-8",
            )
            indices, offsets, scores, buckets = export_bullet_v4.load_validation(path, 0, 0)
        self.assertEqual(scores.size, 2)
        self.assertEqual(buckets.size, 2)
        self.assertEqual(int(scores[0]), 17)
        self.assertEqual(int(scores[1]), 31)  # black to move: white-relative -31 flips sign

    def test_truncated_dataset_header_raises_trainer_error(self) -> None:
        import struct

        feature_set = b"halfka-king-bucket-v1"
        blob = struct.pack("<8sIH", b"KOI-DATA", 1, len(feature_set)) + feature_set + struct.pack("<Q", 0)
        self.assertEqual(len(blob), 43)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "dataset.bin"
            path.write_bytes(blob[:-1])
            with self.assertRaises(export_bullet_v4.train_nnue_koi.TrainerError):
                export_bullet_v4.train_nnue_koi.load_binary_dataset(path, 0)


@unittest.skipUnless(BULLET_TOOLING_AVAILABLE, "numpy and the bullet tooling are required")
@unittest.skipUnless(CHESS_AVAILABLE, "python-chess is required for the exporter")
class ExporterV5Tests(unittest.TestCase):
    def test_infer_hidden_units_v5(self) -> None:
        l1 = 8
        size = 32 * export_bullet_v5.bytes_per_hidden(l1) + export_bullet_v5.tail_bytes(l1)
        self.assertEqual(export_bullet_v5.infer_hidden_units(size, l1), 32)
        with self.assertRaises(export_bullet_v5.ExportError):
            export_bullet_v5.infer_hidden_units(size + 4, l1)
        with self.assertRaises(export_bullet_v5.ExportError):
            export_bullet_v5.infer_hidden_units(size, l1 + 1)

    def test_read_raw_weights_v5_layout(self) -> None:
        hidden = 32
        l1 = 8
        feature_major = np.arange(36864 * hidden, dtype=np.float32).reshape(36864, hidden) / 1000.0
        hidden_bias = np.linspace(-1.0, 1.0, hidden, dtype=np.float32)
        l1_unit = np.arange(l1 * hidden, dtype=np.float32).reshape(l1, hidden) / 100.0
        l1_bias = np.linspace(-0.5, 0.5, l1, dtype=np.float32)
        output_bucket = np.arange(8 * l1, dtype=np.float32).reshape(8, l1) / 100.0
        output_bias = np.linspace(-0.25, 0.25, 8, dtype=np.float32)
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            raw.write_bytes(
                feature_major.astype("<f4").tobytes()
                + hidden_bias.astype("<f4").tobytes()
                + l1_unit.T.astype("<f4").tobytes()
                + l1_bias.astype("<f4").tobytes()
                + output_bucket.T.astype("<f4").tobytes()
                + output_bias.astype("<f4").tobytes()
            )
            weights = export_bullet_v5.read_raw_weights(raw, hidden, l1)
        self.assertEqual(weights["feature_weights"].shape, (36864, hidden))
        self.assertEqual(weights["l1_weights"].shape, (l1, hidden))
        self.assertEqual(weights["output_weights"].shape, (8, l1))
        np.testing.assert_allclose(weights["feature_weights"], feature_major, rtol=0, atol=1e-3)
        np.testing.assert_allclose(weights["hidden_bias"], hidden_bias, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["l1_weights"], l1_unit, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["l1_bias"], l1_bias, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["output_weights"], output_bucket, rtol=0, atol=1e-6)
        np.testing.assert_allclose(weights["output_bias"], output_bias, rtol=0, atol=1e-6)

    def test_exporter_writes_a_v5_container(self) -> None:
        import json

        hidden = 32
        l1 = 8
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            raw.write_bytes(
                b"\x00"
                * (hidden * export_bullet_v5.bytes_per_hidden(l1) + export_bullet_v5.tail_bytes(l1))
            )
            validation = Path(temp) / "validation.txt"
            validation.write_text(
                f"{STARTPOS} | 17 | 0.5\n{BLACK_TO_MOVE} | -31 | 0.5\n",
                encoding="utf-8",
            )
            net = Path(temp) / "koi-v5.nnue"
            meta = Path(temp) / "koi-v5.metadata.json"
            exit_code = export_bullet_v5.main([
                "--checkpoint", str(raw),
                "--validation", str(validation),
                "--net-out", str(net),
                "--meta-out", str(meta),
                "--hidden", str(hidden),
                "--l1-units", str(l1),
                "--hidden-shifts", "7",
                "--l1-shifts", "6",
                "--output-shifts", "12",
                "--tune-samples", "2",
            ])
            self.assertEqual(exit_code, 0)
            data = net.read_bytes()
            header = struct.Struct("<8sIIIIIBBBBHHQ")
            (magic, version, input_units, hidden_units, buckets, l1_units,
             hidden_shift, output_shift, l1_shift, reserved, qlen, flen, payload_len) = header.unpack_from(data, 0)
            self.assertEqual(magic, b"KOI-NNUE")
            self.assertEqual((version, input_units, hidden_units, buckets, l1_units), (5, 36864, 32, 8, 8))
            self.assertEqual((hidden_shift, output_shift, l1_shift, reserved), (7, 12, 6, 0))
            self.assertEqual(qlen, 10)
            self.assertEqual(flen, 37)
            expected_payload = 36864 * hidden * 2 + hidden * 4 + l1 * hidden + l1 * 4 + 8 * l1 + 8 * 4
            self.assertEqual(payload_len, expected_payload)
            self.assertEqual(len(data), 44 + 32 + qlen + flen + expected_payload)
            metadata = json.loads(meta.read_text(encoding="utf-8"))
            self.assertEqual(metadata["schema"], "koi-nnue-training-metadata-v3")
            self.assertEqual(metadata["backend"], "bullet")
            self.assertEqual(metadata["architecture"], {
                "input": 36864,
                "hidden": 32,
                "l1": 8,
                "output_buckets": 8,
                "groups": ["halfka-king-bucket-v1", "threat-pairs-v1"],
            })

    def test_exporter_rejects_a_hidden_mismatch_v5(self) -> None:
        l1 = 8
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            raw.write_bytes(
                b"\x00"
                * (32 * export_bullet_v5.bytes_per_hidden(l1) + export_bullet_v5.tail_bytes(l1))
            )
            exit_code = export_bullet_v5.main([
                "--checkpoint", str(raw),
                "--hidden", "64",
                "--l1-units", str(l1),
                "--net-out", str(Path(temp) / "net.nnue"),
                "--meta-out", str(Path(temp) / "net.metadata.json"),
            ])
        self.assertEqual(exit_code, 2)

    def test_exporter_without_validation_selects_the_first_shifts(self) -> None:
        hidden = 32
        l1 = 8
        with tempfile.TemporaryDirectory() as temp:
            raw = Path(temp) / "raw.bin"
            raw.write_bytes(
                b"\x00"
                * (hidden * export_bullet_v5.bytes_per_hidden(l1) + export_bullet_v5.tail_bytes(l1))
            )
            net = Path(temp) / "koi-v5.nnue"
            exit_code = export_bullet_v5.main([
                "--checkpoint", str(raw),
                "--net-out", str(net),
                "--meta-out", str(Path(temp) / "koi-v5.metadata.json"),
                "--hidden", str(hidden),
                "--l1-units", str(l1),
                "--hidden-shifts", "7",
                "--l1-shifts", "6",
                "--output-shifts", "12",
            ])
            self.assertEqual(exit_code, 0)
            self.assertTrue(net.exists())


if __name__ == "__main__":
    unittest.main()
