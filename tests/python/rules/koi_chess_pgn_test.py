"""Unit tests for koi_chess.pgn.

These tests pin the python-chess PGN behaviors Koi's tooling relies on:
default headers, FEN setup games, comment/NAG attachment, variation skipping,
result handling, illegal-SAN reporting and multi-game streams.
"""

import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "measurement"))

import koi_chess  # noqa: E402
from koi_chess import pgn  # noqa: E402

STARTING_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


def read_games(text):
    stream = io.StringIO(text)
    games = []
    while True:
        game = pgn.read_game(stream)
        if game is None:
            return games
        games.append(game)


def mainline_ucis(game):
    return [node.move.uci() for node in game.mainline()]


class HeaderTests(unittest.TestCase):
    def test_default_headers_and_order(self):
        game = read_games('[Event "Test"]\n')[0]
        self.assertEqual(
            list(game.headers),
            ["Event", "Site", "Date", "Round", "White", "Black", "Result"],
        )
        self.assertEqual(game.headers["Event"], "Test")
        self.assertEqual(game.headers["Site"], "?")
        self.assertEqual(game.headers["Date"], "????.??.??")
        self.assertEqual(game.headers["Round"], "?")
        self.assertEqual(game.headers["White"], "?")
        self.assertEqual(game.headers["Black"], "?")
        self.assertEqual(game.headers["Result"], "*")

    def test_extra_headers_append_in_file_order(self):
        game = read_games('[Event "A"]\n[SetUp "1"]\n[FEN "8/8/8/8/8/8/8/K6k w - - 0 1"]\n')[0]
        self.assertEqual(list(game.headers)[:7], ["Event", "Site", "Date", "Round", "White", "Black", "Result"])
        self.assertEqual(list(game.headers)[7:], ["SetUp", "FEN"])

    def test_header_keys_are_case_sensitive(self):
        game = read_games('[event "A"]\n[Event "B"]\n')[0]
        self.assertEqual(game.headers["Event"], "B")
        self.assertEqual(game.headers["event"], "A")
        self.assertEqual(list(game.headers)[7:], ["event"])

    def test_duplicate_header_last_wins(self):
        game = read_games('[Event "A"]\n[Event "B"]\n')[0]
        self.assertEqual(game.headers["Event"], "B")


class ResultTests(unittest.TestCase):
    def test_result_token_sets_header(self):
        for token in ("1-0", "0-1", "1/2-1/2", "*"):
            with self.subTest(token=token):
                game = read_games(f"1. e4 {token}\n")[0]
                self.assertEqual(game.headers["Result"], token)

    def test_explicit_result_header_wins(self):
        game = read_games('[Result "0-1"]\n\n1. e4 1-0\n')[0]
        self.assertEqual(game.headers["Result"], "0-1")

    def test_missing_result_token_keeps_default(self):
        game = read_games("1. e4 e5\n")[0]
        self.assertEqual(game.headers["Result"], "*")


class FenSetupTests(unittest.TestCase):
    def test_fen_header_creates_the_root_position(self):
        fen = "7k/8/8/8/8/8/8/7K b - - 7 23"
        game = read_games(f'[FEN "{fen}"]\n\n23... Kg8 *\n')[0]
        self.assertEqual(game.board().fen(), fen)
        self.assertEqual(game.board().fullmove_number, 23)
        self.assertEqual(mainline_ucis(game), ["h8g8"])
        self.assertFalse(game.errors)

    def test_setup_zero_still_applies_the_fen_header(self):
        fen = "8/8/8/8/8/8/8/K6k w - - 0 1"
        game = read_games(f'[SetUp "0"]\n[FEN "{fen}"]\n\n*')[0]
        self.assertEqual(game.board().fen(), fen)

    def test_invalid_fen_is_reported_and_keeps_the_starting_position(self):
        game = read_games('[FEN "not a fen"]\n\n1. e4 *\n')[0]
        self.assertTrue(game.errors)
        self.assertIsInstance(game.errors[0], ValueError)
        self.assertEqual(game.board().fen(), STARTING_FEN)
        self.assertEqual(list(game.mainline()), [])


class MovetextTests(unittest.TestCase):
    def test_mainline_excludes_the_root(self):
        game = read_games("1. e4 e5 2. Nf3 *\n")[0]
        nodes = list(game.mainline())
        self.assertEqual([node.move.uci() for node in nodes], ["e2e4", "e7e5", "g1f3"])
        self.assertIsNone(nodes[0].parent.move)
        self.assertEqual(nodes[1].parent.move.uci(), "e2e4")
        self.assertEqual(nodes[0].comment, "")
        self.assertEqual(nodes[0].nags, [])

    def test_move_numbers_with_and_without_spaces(self):
        game = read_games("1.e4 e5 2. Nf3 2... Nc6 3. Bc4 *\n")[0]
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4"])

    def test_black_ellipsis_move_number(self):
        fen = "7k/8/8/8/8/8/8/7K b - - 7 23"
        game = read_games(f'[FEN "{fen}"]\n\n23... Kg8 24. Kg1 *\n')[0]
        self.assertFalse(game.errors)
        self.assertEqual(mainline_ucis(game), ["h8g8", "h1g1"])

    def test_comments_and_nags_attach_to_the_previous_move(self):
        game = read_games("1. e4 $1 {first} e5 {second} $2 $3 *\n")[0]
        nodes = list(game.mainline())
        self.assertEqual(nodes[0].nags, [1])
        self.assertEqual(nodes[0].comment, "first")
        self.assertEqual(nodes[1].comment, "second")
        self.assertEqual(nodes[1].nags, [2, 3])

    def test_multiple_comments_are_joined(self):
        game = read_games("1. e4 {a} {b} e5 *\n")[0]
        self.assertEqual(list(game.mainline())[0].comment, "a b")

    def test_comment_before_any_move_is_ignored(self):
        game = read_games("{hello} 1. e4 e5 *\n")[0]
        self.assertEqual(list(game.mainline())[0].comment, "")
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5"])

    def test_variations_are_skipped(self):
        game = read_games("1. e4 (1. d4 d5) e5 2. Nf3 *\n")[0]
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5", "g1f3"])

    def test_nested_variations_are_skipped(self):
        game = read_games("1. e4 (1. d4 (1. c4 c5) d5) e5 *\n")[0]
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5"])

    def test_semicolon_comment_is_ignored(self):
        game = read_games("1. e4 ; note\n1... e5 *\n")[0]
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5"])

    def test_multiline_comment_keeps_blank_lines(self):
        game = read_games("1. e4 {a\n\nb} e5 *\n")[0]
        self.assertEqual(list(game.mainline())[0].comment, "a\n\nb")

    def test_blank_line_inside_variation_ends_the_game(self):
        game = read_games("1. e4 (1. d4\n\nd5) e5 *\n")[0]
        self.assertEqual(mainline_ucis(game), ["e2e4"])

    def test_illegal_move_is_reported_and_stops_the_mainline(self):
        game = read_games("1. e4 e5 2. Ke2 Ke7 3. e9 *\n")[0]
        self.assertTrue(game.errors)
        self.assertEqual(mainline_ucis(game), ["e2e4", "e7e5", "e1e2", "e8e7"])

    def test_illegal_first_move_reports_an_error(self):
        game = read_games("1. e9 *\n")[0]
        self.assertTrue(game.errors)
        self.assertEqual(list(game.mainline()), [])

    def test_percent_escape_lines_are_skipped(self):
        game = read_games('% escape\n[Event "A"]\n\n1. e4 *\n')[0]
        self.assertEqual(game.headers["Event"], "A")
        self.assertEqual(mainline_ucis(game), ["e2e4"])

    def test_elo_oracle_fixture(self):
        text = (
            '[Event "Test"]\n[Site "?"]\n[Date "????.??.??"]\n[Round "?"]\n'
            '[White "White"]\n[Black "Black"]\n[Result "*"]\n\n'
            "1. e4 {king pawn} (1. d4 d5) e5 2. Nf3 Nc6 3. Bc4 Nf6 4. O-O Be7 *\n"
        )
        game = read_games(text)[0]
        nodes = list(game.mainline())
        self.assertEqual(len(nodes), 8)
        self.assertEqual(nodes[0].move.uci(), "e2e4")
        self.assertEqual(nodes[6].move.uci(), "e1g1")
        self.assertNotIn("d2d4", mainline_ucis(game))
        board = game.board()
        self.assertEqual(board.san(nodes[0].move), "e4")
        self.assertEqual(board.fen(), STARTING_FEN)


class StreamTests(unittest.TestCase):
    def test_empty_stream_returns_none(self):
        self.assertIsNone(pgn.read_game(io.StringIO("")))
        self.assertIsNone(pgn.read_game(io.StringIO("\n\n")))
        self.assertIsNone(pgn.read_game(io.StringIO("% only\n")))

    def test_multiple_games_are_separated_by_blank_lines(self):
        text = '[Event "A"]\n\n1. e4 *\n\n[Event "B"]\n\n1. d4 *\n'
        games = read_games(text)
        self.assertEqual(len(games), 2)
        self.assertEqual(games[0].headers["Event"], "A")
        self.assertEqual(games[1].headers["Event"], "B")
        self.assertEqual(mainline_ucis(games[0]), ["e2e4"])
        self.assertEqual(mainline_ucis(games[1]), ["d2d4"])

    def test_game_board_returns_an_isolated_copy(self):
        game = read_games("1. e4 *\n")[0]
        board = game.board()
        board.push(koi_chess.Move.from_uci("e7e5"))
        self.assertEqual(game.board().fen(), STARTING_FEN)

    def test_pgn_module_is_exported(self):
        self.assertIs(koi_chess.pgn, pgn)
        self.assertTrue(hasattr(pgn, "Game"))
        self.assertTrue(hasattr(pgn, "Node"))


if __name__ == "__main__":
    unittest.main()
