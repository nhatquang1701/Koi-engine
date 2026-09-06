import contextlib
import io
import unittest


class StockfishMatchStrengthTest(unittest.TestCase):
    def setUp(self):
        import tools.stockfish_match as stockfish_match

        self.stockfish_match = stockfish_match
        self.parser = stockfish_match._build_parser()
        self.base_arguments = [
            "--koi",
            r"C:\Koi Engine\koi.exe",
            "--stockfish",
            r"C:\Engines\stockfish.exe",
        ]

    def test_default_match_does_not_pass_opponent_strength(self):
        args = self.parser.parse_args(self.base_arguments)

        command = self.stockfish_match.build_command(args, powershell="pwsh-test")

        self.assertNotIn("-OpponentElo", command)

    def test_default_match_preserves_hash_default(self):
        args = self.parser.parse_args(self.base_arguments)

        command = self.stockfish_match.build_command(args, powershell="pwsh-test")

        self.assertEqual(command[command.index("-Hash") + 1], "16")

    def test_explicit_opponent_elo_is_forwarded_to_powershell(self):
        args = self.parser.parse_args(self.base_arguments + ["--opponent-elo", "2100"])

        command = self.stockfish_match.build_command(args, powershell="pwsh-test")

        self.assertEqual(args.opponent_elo, 2100)
        self.assertEqual(command[command.index("-OpponentElo") + 1], "2100")

    def test_opponent_elo_is_clamped_to_stockfish_limits(self):
        for requested, expected in ((0, "1320"), (1000, "1320"), (4000, "3190")):
            with self.subTest(requested=requested):
                args = self.parser.parse_args(
                    self.base_arguments + ["--opponent-elo", str(requested)]
                )

                command = self.stockfish_match.build_command(args, powershell="pwsh-test")

                self.assertEqual(
                    command[command.index("-OpponentElo") + 1],
                    expected,
                )

    def test_malformed_opponent_elo_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parser.parse_args(
                    self.base_arguments + ["--opponent-elo", "not-an-elo"]
                )


if __name__ == "__main__":
    unittest.main()
