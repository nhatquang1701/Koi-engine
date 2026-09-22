"""Bitboard board state, move generation, SAN and game termination rules.

This is the workhorse of the koi_chess compatibility library.  Positions are
stored as twelve piece bitboards plus castling rights, en passant square and
clocks; move generation is pseudo-legal generation followed by an apply/undo
legality filter, and SAN is generated from the legal move list.
"""

from __future__ import annotations

import enum
import re

from .attacks import (
    BISHOP_RAYS,
    KING_ATTACKS,
    KNIGHT_ATTACKS,
    PAWN_ATTACKS,
    ROOK_RAYS,
    sliding_attacks,
)
from .core import (
    BB_DARK_SQUARES,
    BB_FILE_A,
    BB_FILE_H,
    BB_LIGHT_SQUARES,
    BB_RANK_1,
    BB_RANK_2,
    BB_RANK_3,
    BB_RANK_6,
    BB_RANK_7,
    BB_RANK_8,
    BB_SQUARES,
    BISHOP,
    BLACK,
    KING,
    KNIGHT,
    PAWN,
    PIECE_SYMBOLS,
    PIECE_TYPES,
    QUEEN,
    ROOK,
    WHITE,
    Move,
    Piece,
    bit_scan,
    iter_bits,
    parse_square,
    square_file,
    square_name,
    square_rank,
)

STARTING_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

_PIECE_BB_NAMES = {
    PAWN: "pawns",
    KNIGHT: "knights",
    BISHOP: "bishops",
    ROOK: "rooks",
    QUEEN: "queens",
    KING: "kings",
}
_PIECE_CHARS = {PAWN: "p", KNIGHT: "n", BISHOP: "b", ROOK: "r", QUEEN: "q", KING: "k"}
_CHAR_PIECES = {char: piece_type for piece_type, char in _PIECE_CHARS.items()}

_A1, _H1, _A8, _H8 = 0, 7, 56, 63
_E1, _F1, _G1, _D1, _C1, _B1 = 4, 5, 6, 3, 2, 1
_E8, _F8, _G8, _D8, _C8, _B8 = 60, 61, 62, 59, 58, 57

WHITE_OO = BB_SQUARES[_H1]
WHITE_OOO = BB_SQUARES[_A1]
BLACK_OO = BB_SQUARES[_H8]
BLACK_OOO = BB_SQUARES[_A8]
ALL_CASTLING_RIGHTS = WHITE_OO | WHITE_OOO | BLACK_OO | BLACK_OOO


def _build_castling_masks():
    masks = []
    for square in range(64):
        mask = ALL_CASTLING_RIGHTS
        if square == _H1:
            mask &= ~WHITE_OO
        if square == _A1:
            mask &= ~WHITE_OOO
        if square == _H8:
            mask &= ~BLACK_OO
        if square == _A8:
            mask &= ~BLACK_OOO
        masks.append(mask)
    return tuple(masks)


_CASTLING_MASKS = _build_castling_masks()

_SAN_SUFFIX_RE = re.compile(r"[+#]+$")
_SAN_GLYPH_RE = re.compile(r"[!?]+$")
_SAN_EP_RE = re.compile(r"\s*e\.?\s*p\.?\s*$", re.IGNORECASE)


class Termination(enum.Enum):
    """The way a game ended, matching the python-chess enumeration values."""

    CHECKMATE = 1
    STALEMATE = 2
    INSUFFICIENT_MATERIAL = 3
    SEVENTYFIVE_MOVES = 4
    FIVEFOLD_REPETITION = 5
    FIFTY_MOVES = 6
    THREEFOLD_REPETITION = 7


class Outcome:
    """The result of a finished game."""

    __slots__ = ("termination", "winner")

    def __init__(self, termination: Termination, winner):
        self.termination = termination
        self.winner = winner

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Outcome):
            return NotImplemented
        return self.termination == other.termination and self.winner == other.winner

    def __hash__(self) -> int:
        return hash((self.termination, self.winner))

    def __repr__(self) -> str:
        return f"Outcome(termination={self.termination!r}, winner={self.winner!r})"


class Board:
    """A chess position with reversible history, mirroring python-chess usage."""

    def __init__(self, fen: str | None = None):
        self.pawns = [0, 0]
        self.knights = [0, 0]
        self.bishops = [0, 0]
        self.rooks = [0, 0]
        self.queens = [0, 0]
        self.kings = [0, 0]
        self.occupied_co = [0, 0]
        self.turn = WHITE
        self.castling_rights = 0
        self.ep_square = None
        self.halfmove_clock = 0
        self.fullmove_number = 1
        self.move_stack = []
        self._undo_stack = []
        self._key_stack = []
        self.set_fen(STARTING_FEN if fen is None else fen)

    # -- piece access ------------------------------------------------------

    def _add_piece(self, square: int, color: bool, piece_type: int) -> None:
        bb = BB_SQUARES[square]
        getattr(self, _PIECE_BB_NAMES[piece_type])[color] |= bb
        self.occupied_co[color] |= bb

    def _remove_piece(self, square: int, color: bool, piece_type: int) -> None:
        bb = BB_SQUARES[square]
        getattr(self, _PIECE_BB_NAMES[piece_type])[color] &= ~bb
        self.occupied_co[color] &= ~bb

    def _piece_type_at(self, square: int):
        bb = BB_SQUARES[square]
        if not self.occupied & bb:
            return None
        for piece_type in PIECE_TYPES:
            name = _PIECE_BB_NAMES[piece_type]
            if getattr(self, name)[WHITE] & bb or getattr(self, name)[BLACK] & bb:
                return piece_type
        return None

    def piece_at(self, square: int) -> Piece | None:
        piece_type = self._piece_type_at(square)
        if piece_type is None:
            return None
        color = WHITE if self.occupied_co[WHITE] & BB_SQUARES[square] else BLACK
        return Piece(piece_type, color)

    def piece_map(self) -> dict:
        result = {}
        for color in (WHITE, BLACK):
            for piece_type in PIECE_TYPES:
                bb = getattr(self, _PIECE_BB_NAMES[piece_type])[color]
                while bb:
                    square = bit_scan(bb)
                    bb &= bb - 1
                    result[square] = Piece(piece_type, color)
        return result

    def king(self, color: bool):
        bb = self.kings[color] & self.occupied_co[color]
        return bit_scan(bb) if bb else None

    @property
    def occupied(self) -> int:
        return self.occupied_co[WHITE] | self.occupied_co[BLACK]

    def ply(self) -> int:
        return len(self.move_stack)

    # -- fen ---------------------------------------------------------------

    def board_fen(self) -> str:
        rows = []
        for rank_index in range(7, -1, -1):
            row = []
            empty = 0
            for file_index in range(8):
                piece = self.piece_at(rank_index * 8 + file_index)
                if piece is None:
                    empty += 1
                    continue
                if empty:
                    row.append(str(empty))
                    empty = 0
                row.append(piece.symbol())
            if empty:
                row.append(str(empty))
            rows.append("".join(row))
        return "/".join(rows)

    def castling_xfen(self) -> str:
        parts = []
        if self.castling_rights & WHITE_OO:
            parts.append("K")
        if self.castling_rights & WHITE_OOO:
            parts.append("Q")
        if self.castling_rights & BLACK_OO:
            parts.append("k")
        if self.castling_rights & BLACK_OOO:
            parts.append("q")
        return "".join(parts) if parts else "-"

    def fen(self) -> str:
        ep_part = "-"
        if self.ep_square is not None and self.has_legal_en_passant():
            ep_part = square_name(self.ep_square)
        turn_part = "w" if self.turn == WHITE else "b"
        return (
            f"{self.board_fen()} {turn_part} {self.castling_xfen()} {ep_part} "
            f"{self.halfmove_clock} {self.fullmove_number}"
        )

    def _set_board_fen(self, board_part: str) -> None:
        for name in _PIECE_BB_NAMES.values():
            setattr(self, name, [0, 0])
        self.occupied_co = [0, 0]
        rows = board_part.split("/")
        if len(rows) != 8:
            raise ValueError(f"expected 8 rows in the board part of the fen: {board_part!r}")
        for row_index, row in enumerate(rows):
            rank_index = 7 - row_index
            file_index = 0
            for char in row:
                if char.isdigit():
                    if char == "0":
                        raise ValueError(f"invalid digit in the board part of the fen: {char!r}")
                    file_index += int(char)
                elif char.lower() in _CHAR_PIECES:
                    if file_index > 7:
                        raise ValueError(f"too many files in the board part of the fen: {board_part!r}")
                    self._add_piece(
                        rank_index * 8 + file_index,
                        WHITE if char.isupper() else BLACK,
                        _CHAR_PIECES[char.lower()],
                    )
                    file_index += 1
                else:
                    raise ValueError(f"invalid piece character in the fen: {char!r}")
            if file_index != 8:
                raise ValueError(
                    f"expected 8 files in rank {8 - rank_index} of the fen: {board_part!r}"
                )

    def _set_castling(self, castling_part: str) -> None:
        rights = 0
        if castling_part == "-":
            rights = 0
        else:
            for char in castling_part:
                if char == "K":
                    rights |= WHITE_OO
                elif char == "Q":
                    rights |= WHITE_OOO
                elif char == "k":
                    rights |= BLACK_OO
                elif char == "q":
                    rights |= BLACK_OOO
                else:
                    raise ValueError(f"invalid castling rights: {castling_part!r}")
        self.castling_rights = rights

    def _set_en_passant(self, ep_part: str) -> None:
        if ep_part == "-":
            self.ep_square = None
            return
        try:
            self.ep_square = parse_square(ep_part)
        except ValueError:
            raise ValueError(f"invalid en passant square: {ep_part!r}") from None

    def set_fen(self, fen: str) -> None:
        if not isinstance(fen, str):
            raise ValueError(f"expected a fen string, got {fen!r}")
        parts = fen.split()
        if not parts:
            raise ValueError("empty fen")
        if len(parts) > 6:
            raise ValueError("expected fen to have at most 6 fields")
        self._set_board_fen(parts[0])
        turn_part = parts[1] if len(parts) > 1 else "w"
        if turn_part == "w":
            self.turn = WHITE
        elif turn_part == "b":
            self.turn = BLACK
        else:
            raise ValueError(f"expected 'w' or 'b' for turn, got {turn_part!r}")
        self._set_castling(parts[2] if len(parts) > 2 else "-")
        self._set_en_passant(parts[3] if len(parts) > 3 else "-")
        halfmove_part = parts[4] if len(parts) > 4 else "0"
        try:
            halfmove_clock = int(halfmove_part)
        except ValueError:
            raise ValueError(f"expected a halfmove clock, got {halfmove_part!r}") from None
        if halfmove_clock < 0:
            raise ValueError("expected a non-negative halfmove clock")
        self.halfmove_clock = halfmove_clock
        fullmove_part = parts[5] if len(parts) > 5 else "1"
        try:
            fullmove_number = int(fullmove_part)
        except ValueError:
            raise ValueError(f"expected a fullmove number, got {fullmove_part!r}") from None
        if fullmove_number < 1:
            raise ValueError("expected a positive fullmove number")
        self.fullmove_number = fullmove_number
        self.move_stack = []
        self._undo_stack = []
        self._key_stack = [self._transposition_key()]

    # -- attacks -----------------------------------------------------------

    def attacks_mask(self, square: int) -> int:
        bb_square = BB_SQUARES[square]
        if bb_square & self.pawns[WHITE]:
            return PAWN_ATTACKS[WHITE][square]
        if bb_square & self.pawns[BLACK]:
            return PAWN_ATTACKS[BLACK][square]
        if bb_square & (self.knights[WHITE] | self.knights[BLACK]):
            return KNIGHT_ATTACKS[square]
        if bb_square & (self.kings[WHITE] | self.kings[BLACK]):
            return KING_ATTACKS[square]
        occupied = self.occupied
        attacks = 0
        if bb_square & (
            self.bishops[WHITE]
            | self.bishops[BLACK]
            | self.queens[WHITE]
            | self.queens[BLACK]
        ):
            attacks |= sliding_attacks(square, occupied, BISHOP_RAYS)
        if bb_square & (
            self.rooks[WHITE]
            | self.rooks[BLACK]
            | self.queens[WHITE]
            | self.queens[BLACK]
        ):
            attacks |= sliding_attacks(square, occupied, ROOK_RAYS)
        return attacks

    def attacks(self, square: int) -> list:
        return list(iter_bits(self.attacks_mask(square)))

    def is_attacked_by(self, color: bool, square: int) -> bool:
        if PAWN_ATTACKS[not color][square] & self.pawns[color]:
            return True
        if KNIGHT_ATTACKS[square] & self.knights[color]:
            return True
        if KING_ATTACKS[square] & self.kings[color]:
            return True
        occupied = self.occupied
        if sliding_attacks(square, occupied, BISHOP_RAYS) & (
            self.bishops[color] | self.queens[color]
        ):
            return True
        if sliding_attacks(square, occupied, ROOK_RAYS) & (
            self.rooks[color] | self.queens[color]
        ):
            return True
        return False

    def is_check(self) -> bool:
        king_square = self.king(self.turn)
        if king_square is None:
            return False
        return self.is_attacked_by(not self.turn, king_square)

    def is_valid(self) -> bool:
        if (self.kings[WHITE] & self.occupied_co[WHITE]).bit_count() != 1:
            return False
        if (self.kings[BLACK] & self.occupied_co[BLACK]).bit_count() != 1:
            return False
        if (self.kings[WHITE] | self.kings[BLACK]) & ~self.occupied:
            return False
        if self.pawns[WHITE] & (BB_RANK_1 | BB_RANK_8):
            return False
        if self.pawns[BLACK] & (BB_RANK_1 | BB_RANK_8):
            return False
        if self.is_attacked_by(self.turn, self.king(not self.turn)):
            return False
        if self.ep_square is not None:
            if not BB_SQUARES[self.ep_square] & (BB_RANK_3 | BB_RANK_6):
                return False
            if not BB_SQUARES[self.ep_square ^ 8] & self.pawns[not self.turn]:
                return False
        for rights, color, king_square, rook_square in (
            (WHITE_OO, WHITE, _E1, _H1),
            (WHITE_OOO, WHITE, _E1, _A1),
            (BLACK_OO, BLACK, _E8, _H8),
            (BLACK_OOO, BLACK, _E8, _A8),
        ):
            if self.castling_rights & rights:
                if not self.kings[color] & BB_SQUARES[king_square]:
                    return False
                if not self.rooks[color] & BB_SQUARES[rook_square]:
                    return False
        return True

    # -- move generation ---------------------------------------------------

    def generate_pseudo_legal_moves(self):
        turn = self.turn
        own = self.occupied_co[turn]
        enemy = self.occupied_co[not turn]
        occupied = self.occupied
        promotion_rank = BB_RANK_8 if turn == WHITE else BB_RANK_1
        start_rank = BB_RANK_2 if turn == WHITE else BB_RANK_7
        forward = 8 if turn == WHITE else -8

        pawns = self.pawns[turn]
        while pawns:
            from_square = bit_scan(pawns)
            pawns &= pawns - 1
            to_square = from_square + forward
            if 0 <= to_square < 64 and not occupied & BB_SQUARES[to_square]:
                if BB_SQUARES[to_square] & promotion_rank:
                    yield Move(from_square, to_square, QUEEN)
                    yield Move(from_square, to_square, ROOK)
                    yield Move(from_square, to_square, BISHOP)
                    yield Move(from_square, to_square, KNIGHT)
                else:
                    yield Move(from_square, to_square)
                    if BB_SQUARES[from_square] & start_rank:
                        double_square = from_square + 2 * forward
                        if not occupied & BB_SQUARES[double_square]:
                            yield Move(from_square, double_square)
            for target in iter_bits(PAWN_ATTACKS[turn][from_square] & enemy):
                if BB_SQUARES[target] & promotion_rank:
                    yield Move(from_square, target, QUEEN)
                    yield Move(from_square, target, ROOK)
                    yield Move(from_square, target, BISHOP)
                    yield Move(from_square, target, KNIGHT)
                else:
                    yield Move(from_square, target)
            if (
                self.ep_square is not None
                and PAWN_ATTACKS[turn][from_square] & BB_SQUARES[self.ep_square]
            ):
                yield Move(from_square, self.ep_square)

        if turn == WHITE:
            if self.castling_rights & WHITE_OO and self.kings[WHITE] & BB_SQUARES[_E1]:
                if not occupied & (BB_SQUARES[_F1] | BB_SQUARES[_G1]) and not (
                    self.is_attacked_by(BLACK, _E1)
                    or self.is_attacked_by(BLACK, _F1)
                    or self.is_attacked_by(BLACK, _G1)
                ):
                    yield Move(_E1, _G1)
            if self.castling_rights & WHITE_OOO and self.kings[WHITE] & BB_SQUARES[_E1]:
                if not occupied & (
                    BB_SQUARES[_D1] | BB_SQUARES[_C1] | BB_SQUARES[_B1]
                ) and not (
                    self.is_attacked_by(BLACK, _E1)
                    or self.is_attacked_by(BLACK, _D1)
                    or self.is_attacked_by(BLACK, _C1)
                ):
                    yield Move(_E1, _C1)
        else:
            if self.castling_rights & BLACK_OO and self.kings[BLACK] & BB_SQUARES[_E8]:
                if not occupied & (BB_SQUARES[_F8] | BB_SQUARES[_G8]) and not (
                    self.is_attacked_by(WHITE, _E8)
                    or self.is_attacked_by(WHITE, _F8)
                    or self.is_attacked_by(WHITE, _G8)
                ):
                    yield Move(_E8, _G8)
            if self.castling_rights & BLACK_OOO and self.kings[BLACK] & BB_SQUARES[_E8]:
                if not occupied & (
                    BB_SQUARES[_D8] | BB_SQUARES[_C8] | BB_SQUARES[_B8]
                ) and not (
                    self.is_attacked_by(WHITE, _E8)
                    or self.is_attacked_by(WHITE, _D8)
                    or self.is_attacked_by(WHITE, _C8)
                ):
                    yield Move(_E8, _C8)

        pieces = self.knights[turn]
        while pieces:
            from_square = bit_scan(pieces)
            pieces &= pieces - 1
            for to_square in iter_bits(KNIGHT_ATTACKS[from_square] & ~own):
                yield Move(from_square, to_square)

        pieces = self.kings[turn]
        while pieces:
            from_square = bit_scan(pieces)
            pieces &= pieces - 1
            for to_square in iter_bits(KING_ATTACKS[from_square] & ~own):
                yield Move(from_square, to_square)

        pieces = self.bishops[turn] | self.queens[turn]
        while pieces:
            from_square = bit_scan(pieces)
            pieces &= pieces - 1
            for to_square in iter_bits(
                sliding_attacks(from_square, occupied, BISHOP_RAYS) & ~own
            ):
                yield Move(from_square, to_square)

        pieces = self.rooks[turn] | self.queens[turn]
        while pieces:
            from_square = bit_scan(pieces)
            pieces &= pieces - 1
            for to_square in iter_bits(
                sliding_attacks(from_square, occupied, ROOK_RAYS) & ~own
            ):
                yield Move(from_square, to_square)

    def generate_legal_moves(self):
        us = self.turn
        them = not us
        for move in self.generate_pseudo_legal_moves():
            undo = self._apply(move)
            king_square = self.king(us)
            legal = king_square is None or not self.is_attacked_by(them, king_square)
            self._undo(move, undo)
            if legal:
                yield move

    @property
    def legal_moves(self) -> list:
        return list(self.generate_legal_moves())

    # -- apply and undo ----------------------------------------------------

    def _apply(self, move: Move):
        us = self.turn
        them = not us
        from_square = move.from_square
        to_square = move.to_square
        piece_type = self._piece_type_at(from_square)
        if piece_type is None:
            raise ValueError(f"no piece at {square_name(from_square)}")
        captured_type = self._piece_type_at(to_square)
        captured_square = to_square
        if (
            piece_type == PAWN
            and self.ep_square is not None
            and to_square == self.ep_square
            and captured_type is None
            and square_file(to_square) != square_file(from_square)
        ):
            captured_square = to_square - 8 if us == WHITE else to_square + 8
            captured_type = PAWN
        undo = (
            captured_type,
            captured_square,
            self.castling_rights,
            self.ep_square,
            self.halfmove_clock,
            self.fullmove_number,
        )
        if captured_type is not None:
            self._remove_piece(captured_square, them, captured_type)
        self._remove_piece(from_square, us, piece_type)
        self._add_piece(to_square, us, move.promotion if move.promotion else piece_type)
        if piece_type == KING and abs(to_square - from_square) == 2:
            if to_square > from_square:
                rook_from, rook_to = from_square + 3, from_square + 1
            else:
                rook_from, rook_to = from_square - 4, from_square - 1
            self._remove_piece(rook_from, us, ROOK)
            self._add_piece(rook_to, us, ROOK)
        self.castling_rights &= _CASTLING_MASKS[from_square] & _CASTLING_MASKS[to_square]
        if piece_type == KING:
            self.castling_rights &= (
                ~(WHITE_OO | WHITE_OOO) if us == WHITE else ~(BLACK_OO | BLACK_OOO)
            )
        self.ep_square = None
        if piece_type == PAWN and abs(to_square - from_square) == 16:
            self.ep_square = (from_square + to_square) // 2
        if piece_type == PAWN or captured_type is not None:
            self.halfmove_clock = 0
        else:
            self.halfmove_clock += 1
        if us == BLACK:
            self.fullmove_number += 1
        self.turn = them
        return undo

    def _undo(self, move: Move, undo) -> None:
        (
            captured_type,
            captured_square,
            castling_rights,
            ep_square,
            halfmove_clock,
            fullmove_number,
        ) = undo
        us = not self.turn
        self.turn = us
        self.castling_rights = castling_rights
        self.ep_square = ep_square
        self.halfmove_clock = halfmove_clock
        self.fullmove_number = fullmove_number
        from_square = move.from_square
        to_square = move.to_square
        placed_type = move.promotion if move.promotion else self._piece_type_at(to_square)
        if placed_type is None:
            raise ValueError(f"no piece to restore at {square_name(to_square)}")
        self._remove_piece(to_square, us, placed_type)
        self._add_piece(from_square, us, PAWN if move.promotion else placed_type)
        if placed_type == KING and abs(to_square - from_square) == 2:
            if to_square > from_square:
                rook_from, rook_to = from_square + 3, from_square + 1
            else:
                rook_from, rook_to = from_square - 4, from_square - 1
            self._remove_piece(rook_to, us, ROOK)
            self._add_piece(rook_from, us, ROOK)
        if captured_type is not None:
            self._add_piece(captured_square, not us, captured_type)

    def push(self, move: Move) -> None:
        if move is None:
            raise ValueError("push() requires a move")
        undo = self._apply(move)
        self._undo_stack.append(undo)
        self.move_stack.append(move)
        self._key_stack.append(self._transposition_key())

    def pop(self) -> Move:
        if not self.move_stack:
            raise IndexError("pop() called on an empty move stack")
        move = self.move_stack.pop()
        undo = self._undo_stack.pop()
        self._key_stack.pop()
        self._undo(move, undo)
        return move

    def copy(self, stack: bool = True) -> "Board":
        board = Board.__new__(Board)
        board.pawns = list(self.pawns)
        board.knights = list(self.knights)
        board.bishops = list(self.bishops)
        board.rooks = list(self.rooks)
        board.queens = list(self.queens)
        board.kings = list(self.kings)
        board.occupied_co = list(self.occupied_co)
        board.turn = self.turn
        board.castling_rights = self.castling_rights
        board.ep_square = self.ep_square
        board.halfmove_clock = self.halfmove_clock
        board.fullmove_number = self.fullmove_number
        if stack:
            board.move_stack = list(self.move_stack)
            board._undo_stack = list(self._undo_stack)
            board._key_stack = list(self._key_stack)
        else:
            board.move_stack = []
            board._undo_stack = []
            board._key_stack = [board._transposition_key()]
        return board

    # -- move properties ---------------------------------------------------

    def is_legal(self, move: Move) -> bool:
        if not isinstance(move, Move):
            return False
        return any(candidate == move for candidate in self.generate_legal_moves())

    def is_capture(self, move: Move) -> bool:
        if not isinstance(move, Move):
            return False
        if self.occupied & BB_SQUARES[move.to_square]:
            return True
        return (
            self.ep_square is not None
            and move.to_square == self.ep_square
            and bool(self.pawns[self.turn] & BB_SQUARES[move.from_square])
            and square_file(move.to_square) != square_file(move.from_square)
        )

    def gives_check(self, move: Move) -> bool:
        undo = self._apply(move)
        checked = self.is_check()
        self._undo(move, undo)
        return checked

    def is_castling(self, move: Move) -> bool:
        if not isinstance(move, Move):
            return False
        if not (self.kings[WHITE] | self.kings[BLACK]) & BB_SQUARES[move.from_square]:
            return False
        return abs(square_file(move.to_square) - square_file(move.from_square)) > 1

    # -- repetition and termination ----------------------------------------

    def _transposition_key(self):
        ep_square = self.ep_square if self.has_legal_en_passant() else None
        return (
            self.pawns[WHITE],
            self.pawns[BLACK],
            self.knights[WHITE],
            self.knights[BLACK],
            self.bishops[WHITE],
            self.bishops[BLACK],
            self.rooks[WHITE],
            self.rooks[BLACK],
            self.queens[WHITE],
            self.queens[BLACK],
            self.kings[WHITE],
            self.kings[BLACK],
            self.turn,
            self.castling_rights,
            ep_square,
        )

    def has_legal_en_passant(self) -> bool:
        if self.ep_square is None:
            return False
        turn = self.turn
        candidates = PAWN_ATTACKS[not turn][self.ep_square] & self.pawns[turn]
        while candidates:
            from_square = bit_scan(candidates)
            candidates &= candidates - 1
            move = Move(from_square, self.ep_square)
            undo = self._apply(move)
            king_square = self.king(turn)
            legal = king_square is None or not self.is_attacked_by(not turn, king_square)
            self._undo(move, undo)
            if legal:
                return True
        return False

    def is_repetition(self, count: int = 3) -> bool:
        if count <= 1:
            return True
        if not self._key_stack:
            return False
        key = self._key_stack[-1]
        return self._key_stack.count(key) >= count

    def is_fivefold_repetition(self) -> bool:
        return self.is_repetition(5)

    def is_fifty_moves(self) -> bool:
        return self.halfmove_clock >= 100 and not self.is_checkmate()

    def is_seventyfive_moves(self) -> bool:
        return self.halfmove_clock >= 150 and not self.is_checkmate()

    def is_insufficient_material(self) -> bool:
        if (
            self.pawns[WHITE]
            or self.pawns[BLACK]
            or self.rooks[WHITE]
            or self.rooks[BLACK]
            or self.queens[WHITE]
            or self.queens[BLACK]
        ):
            return False
        knights = self.knights[WHITE] | self.knights[BLACK]
        bishops = self.bishops[WHITE] | self.bishops[BLACK]
        if not knights and not bishops:
            return True
        if not bishops and knights.bit_count() == 1:
            return True
        if not knights and (
            not (bishops & BB_DARK_SQUARES) or not (bishops & BB_LIGHT_SQUARES)
        ):
            return True
        return False

    def is_checkmate(self) -> bool:
        return self.is_check() and not any(self.generate_legal_moves())

    def is_stalemate(self) -> bool:
        return not self.is_check() and not any(self.generate_legal_moves())

    def is_game_over(self, claim_draw: bool = False) -> bool:
        if not any(self.generate_legal_moves()):
            return True
        if self.is_insufficient_material():
            return True
        if self.is_seventyfive_moves():
            return True
        if self.is_fivefold_repetition():
            return True
        if claim_draw:
            if self.is_fifty_moves():
                return True
            if self.is_repetition(3):
                return True
        return False

    def outcome(self, claim_draw: bool = False) -> Outcome | None:
        if self.is_checkmate():
            return Outcome(Termination.CHECKMATE, not self.turn)
        if self.is_stalemate():
            return Outcome(Termination.STALEMATE, None)
        if self.is_insufficient_material():
            return Outcome(Termination.INSUFFICIENT_MATERIAL, None)
        if self.is_seventyfive_moves():
            return Outcome(Termination.SEVENTYFIVE_MOVES, None)
        if self.is_fivefold_repetition():
            return Outcome(Termination.FIVEFOLD_REPETITION, None)
        if claim_draw:
            if self.is_fifty_moves():
                return Outcome(Termination.FIFTY_MOVES, None)
            if self.is_repetition(3):
                return Outcome(Termination.THREEFOLD_REPETITION, None)
        return None

    # -- san ---------------------------------------------------------------

    def san(self, move: Move) -> str:
        if not isinstance(move, Move):
            raise ValueError("san() requires a move")
        if self.is_castling(move):
            san = "O-O" if square_file(move.to_square) == 6 else "O-O-O"
        else:
            piece_type = self._piece_type_at(move.from_square)
            if piece_type is None:
                raise ValueError(f"no piece at {square_name(move.from_square)}")
            is_capture = self.is_capture(move)
            if piece_type == PAWN:
                san = square_name(move.from_square)[0] if is_capture else ""
            else:
                san = PIECE_SYMBOLS[piece_type]
                others = [
                    candidate
                    for candidate in self.generate_legal_moves()
                    if candidate != move
                    and candidate.to_square == move.to_square
                    and self._piece_type_at(candidate.from_square) == piece_type
                ]
                if others:
                    same_file = any(
                        square_file(candidate.from_square) == square_file(move.from_square)
                        for candidate in others
                    )
                    same_rank = any(
                        square_rank(candidate.from_square) == square_rank(move.from_square)
                        for candidate in others
                    )
                    if not same_file:
                        san += square_name(move.from_square)[0]
                    elif not same_rank:
                        san += square_name(move.from_square)[1]
                    else:
                        san += square_name(move.from_square)
            if is_capture:
                san += "x"
            san += square_name(move.to_square)
            if move.promotion:
                san += "=" + PIECE_SYMBOLS[move.promotion]
        after = self.copy(stack=False)
        after.push(move)
        if after.is_check():
            san += "#" if not after.legal_moves else "+"
        return san

    def parse_san(self, san: str) -> Move:
        if not isinstance(san, str):
            raise ValueError(f"expected a san string, got {san!r}")
        token = san.strip()
        token = _SAN_EP_RE.sub("", token)
        token = _SAN_GLYPH_RE.sub("", token)
        token = _SAN_SUFFIX_RE.sub("", token)
        if token in ("0-0", "0-0-0"):
            token = token.replace("0", "O")
        for move in self.generate_legal_moves():
            candidate = _SAN_SUFFIX_RE.sub("", self.san(move))
            if candidate == token:
                return move
            if "=" in candidate and candidate.replace("=", "") == token:
                return move
        raise ValueError(f"invalid san: {san!r}")

    # -- misc --------------------------------------------------------------

    def __str__(self) -> str:
        lines = []
        for rank_index in range(7, -1, -1):
            row = []
            for file_index in range(8):
                piece = self.piece_at(rank_index * 8 + file_index)
                row.append(piece.symbol() if piece else ".")
            lines.append(" ".join(row))
        lines.append("Turn: " + ("white" if self.turn == WHITE else "black"))
        return "\n".join(lines)

    def __repr__(self) -> str:
        return f"Board({self.fen()!r})"
