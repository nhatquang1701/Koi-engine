import contextlib
import io
import unittest

from tools import stockfish_match


class StockfishMatchOptionsTests(unittest.TestCase):
    def setUp(self):
        self.parser = stockfish_match._build_parser()
        self.base = ["--koi", r"C:\Koi\koi.exe", "--stockfish", r"C:\Stockfish\stockfish.exe"]

    def test_defaults_keep_hash_16_and_send_book_random_false_for_koi(self):
        command = stockfish_match.build_command(self.parser.parse_args(self.base), powershell="pwsh-test")
        self.assertEqual(command[command.index("-Hash") + 1], "16")
        self.assertEqual(command[command.index("-KoiBookRandom") + 1], "false")
        self.assertNotIn("-OpponentElo", command)

    def test_book_random_and_explicit_opponent_elo_are_forwarded_and_clamped(self):
        args = self.parser.parse_args(self.base + ["--book-random", "true", "--opponent-elo", "0"])
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(command[command.index("-KoiBookRandom") + 1], "true")
        self.assertEqual(command[command.index("-OpponentElo") + 1], "1320")

    def test_explicit_opponent_elo_is_forwarded(self):
        args = self.parser.parse_args(self.base + ["--opponent-elo", "2100"])
        command = stockfish_match.build_command(args, powershell="pwsh-test")
        self.assertEqual(command[command.index("-OpponentElo") + 1], "2100")

    def test_invalid_book_random_and_opponent_elo_are_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--book-random", "maybe"])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.parser.parse_args(self.base + ["--opponent-elo", "not-an-elo"])


if __name__ == "__main__":
    unittest.main()
