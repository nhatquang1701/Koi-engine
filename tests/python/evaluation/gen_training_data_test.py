import sys
import tempfile
import unittest
from pathlib import Path

try:
    import chess
except ImportError:  # pragma: no cover - optional dependency
    chess = None

if chess is None:  # pragma: no cover - optional dependency
    raise unittest.SkipTest("python-chess is unavailable")

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "measurement"))
import gen_training_data  # noqa: E402


class GenTrainingDataTest(unittest.TestCase):
    def test_parser_defaults_match_documented_values(self):
        args = gen_training_data.build_parser().parse_args(["label"])
        self.assertEqual(args.game_depth, 4)
        self.assertEqual(args.label_depth, 9)
        self.assertEqual(args.games, 2000)
        self.assertEqual(args.label_hash, 64)
        self.assertAlmostEqual(args.noise_fraction, 0.15)
        self.assertFalse(args.resume)

    def test_resume_seeds_dedup_from_existing_positions(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "positions.txt"
            fens = [
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1",
                "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
            ]
            path.write_text("\n".join(fens) + "\n\n", encoding="utf-8")
            seen = gen_training_data.load_seen_hashes(path)
            expected = {chess.polyglot.zobrist_hash(chess.Board(fen)) for fen in fens}
            self.assertEqual(seen, expected)
            self.assertEqual(gen_training_data.load_seen_hashes(Path(temporary) / "missing.txt"), set())

    def test_label_resume_reader_parses_rows(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "labels.txt"
            path.write_text(
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1;12;e2e4\n"
                "\n"
                "4k3/8/8/8/8/8/8/4K3 b - - 0 1;-8;e8e7;0.5\n",
                encoding="utf-8",
            )
            self.assertEqual(
                gen_training_data.load_labeled_fens(path),
                {"4k3/8/8/8/8/8/P7/4K3 w - - 0 1", "4k3/8/8/8/8/8/8/4K3 b - - 0 1"},
            )
            self.assertEqual(
                gen_training_data.load_labeled_fens(Path(temporary) / "missing.txt"), set()
            )

    def test_game_result_tokens_are_white_relative(self):
        self.assertEqual(
            gen_training_data.game_result(chess.Board("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1")),
            "1.0",
        )
        self.assertEqual(
            gen_training_data.game_result(chess.Board("7K/6q1/6k1/8/8/8/8/8 w - - 0 1")),
            "0.0",
        )
        self.assertEqual(
            gen_training_data.game_result(chess.Board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")),
            "0.5",
        )

    def test_resume_reader_accepts_result_columns(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "positions.txt"
            fens = [
                "4k3/8/8/8/8/8/P7/4K3 w - - 0 1",
                "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
            ]
            path.write_text(f"{fens[0]};1.0\n{fens[1]};0.5\n", encoding="utf-8")
            seen = gen_training_data.load_seen_hashes(path)
            expected = {chess.polyglot.zobrist_hash(chess.Board(fen)) for fen in fens}
            self.assertEqual(seen, expected)

    def test_consumers_ignore_the_optional_result_column(self):
        import koi_dataset

        row = koi_dataset.parse_row(b"4k3/8/8/8/8/8/P7/4K3 w - - 0 1;12;e2e4;1.0")
        self.assertIsNotNone(row)
        board, cp = row
        self.assertEqual(cp, 12)
        self.assertEqual(board.fen(), "4k3/8/8/8/8/8/P7/4K3 w - - 0 1")

    def test_move_weight_prefers_captures_checks_and_promotions(self):
        quiet = chess.Board()
        self.assertEqual(gen_training_data.move_weight(quiet, chess.Move.from_uci("e2e4")), 1)

        check = chess.Board("4k3/8/8/8/8/8/8/4K2R w K - 0 1")
        self.assertEqual(gen_training_data.move_weight(check, chess.Move.from_uci("h1h8")), 3)

        capture = chess.Board("4k3/8/8/8/8/8/1p6/1R2K3 w - - 0 1")
        self.assertEqual(gen_training_data.move_weight(capture, chess.Move.from_uci("b1b2")), 4)

        promotion = chess.Board("4k3/P7/8/8/8/8/8/4K3 w - - 0 1")
        self.assertEqual(gen_training_data.move_weight(promotion, chess.Move.from_uci("a7a8n")), 5)

        capture_promotion = chess.Board("1n2k3/P7/8/8/8/8/8/4K3 w - - 0 1")
        self.assertEqual(
            gen_training_data.move_weight(capture_promotion, chess.Move.from_uci("a7b8n")), 8
        )


if __name__ == "__main__":
    unittest.main()
