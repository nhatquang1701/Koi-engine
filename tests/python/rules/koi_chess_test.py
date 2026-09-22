"""Unit tests for the koi_chess compatibility library.

koi_chess is the standard-library-only replacement for the python-chess
surfaces Koi's Python tooling used.  These tests pin FEN handling, attack
generation, move generation (through perft), SAN, game termination and the
value types, without depending on any third-party package.
"""

import random
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools" / "measurement"))

import koi_chess as chess  # noqa: E402

STARTING_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
KIWIPETE = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
POSITION_3 = "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"
POSITION_4 = "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1"
POSITION_5 = "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8"
POSITION_6 = "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10"
CASTLING_FEN = "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"


def perft(board, depth):
    if depth == 0:
        return 1
    total = 0
    for move in board.generate_legal_moves():
        board.push(move)
        total += perft(board, depth - 1)
        board.pop()
    return total


class FenTests(unittest.TestCase):
    def test_starting_position_round_trips(self):
        board = chess.Board()
        self.assertEqual(board.fen(), STARTING_FEN)
        self.assertEqual(chess.Board(STARTING_FEN).fen(), STARTING_FEN)

    def test_fen_round_trips_for_standard_positions(self):
        for fen in (STARTING_FEN, KIWIPETE, POSITION_3, POSITION_4, POSITION_5, POSITION_6, CASTLING_FEN):
            with self.subTest(fen=fen):
                self.assertEqual(chess.Board(fen).fen(), fen)

    def test_partial_fen_fields_default(self):
        board = chess.Board("4k3/8/8/8/8/8/8/4K3")
        self.assertEqual(board.turn, chess.WHITE)
        self.assertEqual(board.castling_xfen(), "-")
        self.assertIsNone(board.ep_square)
        self.assertEqual(board.halfmove_clock, 0)
        self.assertEqual(board.fullmove_number, 1)
        self.assertEqual(board.fen(), "4k3/8/8/8/8/8/8/4K3 w - - 0 1")

    def test_en_passant_field_only_written_when_capture_is_legal(self):
        board = chess.Board()
        board.push(chess.Move.from_uci("e2e4"))
        self.assertEqual(
            board.fen(), "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1"
        )
        board.push(chess.Move.from_uci("a7a6"))
        board.push(chess.Move.from_uci("e4e5"))
        board.push(chess.Move.from_uci("d7d5"))
        self.assertEqual(
            board.fen(),
            "rnbqkbnr/1pp1pppp/p7/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3",
        )
        board.push(chess.Move.from_uci("g1f3"))
        self.assertEqual(
            board.fen(),
            "rnbqkbnr/1pp1pppp/p7/3pP3/8/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 3",
        )

    def test_en_passant_field_hidden_when_capture_is_pinned(self):
        board = chess.Board("8/8/8/8/k1Pp3R/8/8/4K3 b - c3 0 2")
        self.assertNotIn("d4c3", {move.uci() for move in board.legal_moves})
        self.assertEqual(board.fen(), "8/8/8/8/k1Pp3R/8/8/4K3 b - - 0 2")

    def test_en_passant_field_kept_when_capture_is_legal(self):
        board = chess.Board("8/8/8/8/k1Pp4/8/8/4K3 b - c3 0 2")
        self.assertIn("d4c3", {move.uci() for move in board.legal_moves})
        self.assertEqual(board.fen(), "8/8/8/8/k1Pp4/8/8/4K3 b - c3 0 2")

    def test_invalid_fen_is_rejected(self):
        for fen in ("", "not a fen", "8/8/8/8/8/8/8 w - - 0 1"):
            with self.subTest(fen=fen):
                with self.assertRaises(ValueError):
                    chess.Board(fen)


class AttackTests(unittest.TestCase):
    def test_starting_position_attack_masks(self):
        board = chess.Board()
        expected = {
            0: 0x102,
            1: 0x50800,
            2: 0xA00,
            3: 0x1C14,
            4: 0x3828,
            7: 0x8040,
            8: 0x20000,
            12: 0x280000,
        }
        for square, mask in expected.items():
            with self.subTest(square=square):
                self.assertEqual(board.attacks_mask(square), mask)

    def test_kiwipete_attack_masks(self):
        board = chess.Board(KIWIPETE)
        expected = {
            0: 0x11E,
            7: 0x8070,
            11: 0x804020140014,
            12: 0x10204280028,
            21: 0x20A070DC7000,
            40: 0x402000204081000,
            52: 0x3828380402000000,
            54: 0xA000A00000000000,
            56: 0x1E01000000000000,
            63: 0x7080808080800000,
        }
        for square, mask in expected.items():
            with self.subTest(square=square):
                self.assertEqual(board.attacks_mask(square), mask)

    def test_attacks_list_matches_mask(self):
        board = chess.Board(KIWIPETE)
        squares = board.attacks(12)
        self.assertEqual(squares, sorted(squares))
        self.assertEqual(sum(1 << square for square in squares), board.attacks_mask(12))

    def test_attacks_mask_of_empty_square_is_zero(self):
        self.assertEqual(chess.Board().attacks_mask(16), 0)

    def test_check_detection(self):
        self.assertFalse(chess.Board(KIWIPETE).is_check())
        self.assertTrue(chess.Board(POSITION_4).is_check())
        self.assertEqual(len(chess.Board(POSITION_4).legal_moves), 6)


class MovegenTests(unittest.TestCase):
    def test_starting_position_has_twenty_moves(self):
        board = chess.Board()
        moves = {move.uci() for move in board.legal_moves}
        self.assertEqual(len(moves), 20)
        self.assertIn("e2e4", moves)
        self.assertIn("g1f3", moves)

    def test_castling_moves_are_generated(self):
        board = chess.Board(CASTLING_FEN)
        moves = {move.uci() for move in board.legal_moves}
        self.assertIn("e1g1", moves)
        self.assertIn("e1c1", moves)
        board.push(chess.Move.from_uci("e1f1"))
        black_moves = {move.uci() for move in board.legal_moves}
        self.assertIn("e8g8", black_moves)
        self.assertIn("e8c8", black_moves)

    def test_castling_rights_are_lost_when_the_king_moves(self):
        board = chess.Board(CASTLING_FEN)
        board.push(chess.Move.from_uci("e1f1"))
        self.assertNotIn("e1g1", {move.uci() for move in board.legal_moves})
        self.assertEqual(board.castling_xfen(), "kq")

    def test_promotions_include_all_pieces(self):
        board = chess.Board("8/P7/8/8/8/8/8/4K2k w - - 0 1")
        moves = {move.uci() for move in board.legal_moves}
        self.assertIn("a7a8q", moves)
        self.assertIn("a7a8r", moves)
        self.assertIn("a7a8b", moves)
        self.assertIn("a7a8n", moves)

    def test_en_passant_capture_is_generated_and_applied(self):
        board = chess.Board("8/8/8/3pP3/8/8/8/4K2k w - d6 0 2")
        board.push(chess.Move.from_uci("e5d6"))
        self.assertEqual(board.fen(), "8/8/3P4/8/8/8/8/4K2k b - - 0 2")

    def test_perft_starting_position(self):
        board = chess.Board()
        for depth, expected in ((1, 20), (2, 400), (3, 8902), (4, 197281)):
            with self.subTest(depth=depth):
                self.assertEqual(perft(board, depth), expected)

    def test_perft_kiwipete(self):
        board = chess.Board(KIWIPETE)
        for depth, expected in ((1, 48), (2, 2039), (3, 97862)):
            with self.subTest(depth=depth):
                self.assertEqual(perft(board, depth), expected)

    def test_perft_position_three(self):
        self.assertEqual(perft(chess.Board(POSITION_3), 3), 2812)

    def test_perft_position_four(self):
        self.assertEqual(perft(chess.Board(POSITION_4), 3), 9467)

    def test_perft_position_five(self):
        self.assertEqual(perft(chess.Board(POSITION_5), 3), 62379)

    def test_perft_position_six(self):
        self.assertEqual(perft(chess.Board(POSITION_6), 3), 89890)


class SanTests(unittest.TestCase):
    def test_castling_san(self):
        board = chess.Board(CASTLING_FEN)
        self.assertEqual(board.san(chess.Move.from_uci("e1g1")), "O-O")
        self.assertEqual(board.san(chess.Move.from_uci("e1c1")), "O-O-O")

    def test_promotion_and_check_san(self):
        board = chess.Board("7k/P7/8/8/8/8/8/6K1 w - - 0 1")
        self.assertEqual(board.san(chess.Move.from_uci("a7a8q")), "a8=Q+")

    def test_mate_san(self):
        board = chess.Board("k7/8/1K6/8/8/8/8/7R w - - 0 1")
        self.assertEqual(board.san(chess.Move.from_uci("h1h8")), "Rh8#")

    def test_file_disambiguation_san(self):
        board = chess.Board("8/8/8/8/8/8/8/N1N1K2k w - - 0 1")
        self.assertEqual(board.san(chess.Move.from_uci("a1b3")), "Nab3")
        self.assertEqual(board.san(chess.Move.from_uci("c1b3")), "Ncb3")

    def test_capture_san(self):
        board = chess.Board("4k3/8/8/8/8/8/1p6/1R2K3 w - - 0 1")
        self.assertEqual(board.san(chess.Move.from_uci("b1b2")), "Rxb2")

    def test_en_passant_san(self):
        board = chess.Board("8/8/8/3pP3/8/8/8/4K2k w - d6 0 2")
        self.assertEqual(board.san(chess.Move.from_uci("e5d6")), "exd6")

    def test_parse_san_round_trips_every_legal_move(self):
        for fen in (STARTING_FEN, KIWIPETE, CASTLING_FEN, "8/P7/8/8/8/8/8/4K2k w - - 0 1"):
            board = chess.Board(fen)
            for move in board.legal_moves:
                with self.subTest(fen=fen, move=move.uci()):
                    self.assertEqual(board.parse_san(board.san(move)), move)

    def test_parse_san_accepts_common_spellings(self):
        board = chess.Board(CASTLING_FEN)
        self.assertEqual(board.parse_san("0-0"), chess.Move.from_uci("e1g1"))
        self.assertEqual(board.parse_san("O-O-O"), chess.Move.from_uci("e1c1"))
        self.assertEqual(
            chess.Board("7k/P7/8/8/8/8/8/6K1 w - - 0 1").parse_san("a8=Q+"),
            chess.Move.from_uci("a7a8q"),
        )


class OutcomeTests(unittest.TestCase):
    def test_checkmate(self):
        board = chess.Board("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1")
        outcome = board.outcome()
        self.assertIsNotNone(outcome)
        self.assertEqual(outcome.termination, chess.Termination.CHECKMATE)
        self.assertEqual(outcome.winner, chess.WHITE)
        self.assertTrue(board.is_checkmate())
        self.assertTrue(board.is_game_over())

    def test_stalemate(self):
        board = chess.Board("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")
        outcome = board.outcome()
        self.assertIsNotNone(outcome)
        self.assertEqual(outcome.termination, chess.Termination.STALEMATE)
        self.assertIsNone(outcome.winner)
        self.assertTrue(board.is_stalemate())

    def test_insufficient_material(self):
        board = chess.Board("8/8/8/8/8/4k3/8/4K3 w - - 0 1")
        outcome = board.outcome()
        self.assertIsNotNone(outcome)
        self.assertEqual(outcome.termination, chess.Termination.INSUFFICIENT_MATERIAL)
        self.assertTrue(board.is_insufficient_material())
        self.assertFalse(chess.Board("8/8/8/8/8/4k3/8/1N2K1N1 w - - 0 1").is_insufficient_material())
        self.assertTrue(chess.Board("8/8/8/8/2b5/4k3/8/3BK3 w - - 0 1").is_insufficient_material())
        self.assertFalse(chess.Board("8/8/8/8/3b4/4k3/8/3BK3 w - - 0 1").is_insufficient_material())

    def test_fifty_move_claim(self):
        board = chess.Board("8/8/8/8/8/4k3/8/4K2R w K - 100 51")
        self.assertTrue(board.is_fifty_moves())
        self.assertIsNone(board.outcome())
        self.assertEqual(board.outcome(claim_draw=True).termination, chess.Termination.FIFTY_MOVES)

    def test_seventyfive_move_rule(self):
        board = chess.Board("8/8/8/8/8/4k3/8/4K2R w K - 150 76")
        outcome = board.outcome()
        self.assertIsNotNone(outcome)
        self.assertEqual(outcome.termination, chess.Termination.SEVENTYFIVE_MOVES)

    def test_threefold_repetition_claim(self):
        board = chess.Board()
        for uci in ("g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1", "f6g8"):
            board.push(chess.Move.from_uci(uci))
        self.assertTrue(board.is_repetition(3))
        self.assertIsNone(board.outcome())
        self.assertEqual(
            board.outcome(claim_draw=True).termination, chess.Termination.THREEFOLD_REPETITION
        )

    def test_fivefold_repetition(self):
        board = chess.Board()
        for _ in range(4):
            for uci in ("g1f3", "g8f6", "f3g1", "f6g8"):
                board.push(chess.Move.from_uci(uci))
        self.assertTrue(board.is_fivefold_repetition())
        self.assertEqual(board.outcome().termination, chess.Termination.FIVEFOLD_REPETITION)


class CoreTypeTests(unittest.TestCase):
    def test_piece_symbols_round_trip(self):
        for symbol in "PNBRQKpnbrqk":
            with self.subTest(symbol=symbol):
                self.assertEqual(chess.Piece.from_symbol(symbol).symbol(), symbol)

    def test_move_uci_round_trip(self):
        for uci in ("e2e4", "a7a8q", "h2h1n"):
            with self.subTest(uci=uci):
                self.assertEqual(chess.Move.from_uci(uci).uci(), uci)
        with self.assertRaises(ValueError):
            chess.Move.from_uci("e2e9")
        with self.assertRaises(ValueError):
            chess.Move.from_uci("e2e4k")

    def test_square_helpers(self):
        for name in ("a1", "e4", "h8"):
            with self.subTest(name=name):
                square = chess.parse_square(name)
                self.assertEqual(chess.square_name(square), name)
        self.assertEqual(chess.square(4, 3), chess.parse_square("e4"))
        self.assertEqual(chess.square_file(chess.parse_square("e4")), 4)
        self.assertEqual(chess.square_rank(chess.parse_square("e4")), 3)

    def test_piece_at_and_piece_map(self):
        board = chess.Board()
        self.assertEqual(board.piece_at(chess.parse_square("e1")).symbol(), "K")
        self.assertIsNone(board.piece_at(chess.parse_square("e4")))
        self.assertEqual(len(board.piece_map()), 32)
        self.assertEqual(len(board.legal_moves), 20)

    def test_push_pop_restores_the_position(self):
        board = chess.Board()
        fen = board.fen()
        board.push(chess.Move.from_uci("e2e4"))
        self.assertNotEqual(board.fen(), fen)
        self.assertEqual(board.pop().uci(), "e2e4")
        self.assertEqual(board.fen(), fen)
        with self.assertRaises(IndexError):
            board.pop()

    def test_copy_isolated_from_the_original(self):
        board = chess.Board()
        copy = board.copy()
        copy.push(chess.Move.from_uci("e2e4"))
        self.assertEqual(board.fen(), STARTING_FEN)
        self.assertNotEqual(copy.fen(), board.fen())


class RandomPlayoutTests(unittest.TestCase):
    def test_legal_playouts_stay_consistent(self):
        rng = random.Random(20260922)
        board = chess.Board()
        for ply in range(120):
            moves = board.legal_moves
            if not moves:
                break
            self.assertTrue(board.is_valid())
            fen = board.fen()
            self.assertEqual(chess.Board(fen).fen(), fen, f"ply {ply}")
            move = rng.choice(moves)
            self.assertTrue(board.is_legal(move))
            board.push(move)
        self.assertGreater(board.ply(), 0)


if __name__ == "__main__":
    unittest.main()
