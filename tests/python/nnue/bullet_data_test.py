"""Tests for the bulletformat dataset converter (tools/nnue/to_bullet.py)."""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[3]
NNUE_DIR = REPO_ROOT / "tools" / "nnue"
if str(NNUE_DIR) not in sys.path:
    sys.path.insert(0, str(NNUE_DIR))

try:
    import chess  # noqa: F401

    CHESS_AVAILABLE = True
except ImportError:  # pragma: no cover - environment dependent
    CHESS_AVAILABLE = False

import run_bullet  # noqa: E402  (also puts tools/measurement on sys.path)
import export_bullet_v4  # noqa: E402
import to_bullet  # noqa: E402

STARTPOS = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
BLACK_TO_MOVE = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"


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


if __name__ == "__main__":
    unittest.main()
