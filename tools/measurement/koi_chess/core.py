"""Core constants and value types for the koi_chess library.

koi_chess is a clean-room, standard-library-only chess toolkit used by Koi's
Python tooling.  It deliberately mirrors the attribute names of the small
python-chess surface the tooling used to depend on, so call sites can switch to
``import koi_chess as chess`` with minimal changes.  No python-chess code is
included, required, or consulted at runtime.
"""

from __future__ import annotations

WHITE = True
BLACK = False

PAWN = 1
KNIGHT = 2
BISHOP = 3
ROOK = 4
QUEEN = 5
KING = 6

PIECE_TYPES = (PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING)
PIECE_SYMBOLS = ("", "P", "N", "B", "R", "Q", "K")
PIECE_NAMES = ("", "pawn", "knight", "bishop", "rook", "queen", "king")

FILE_NAMES = ("a", "b", "c", "d", "e", "f", "g", "h")
RANK_NAMES = ("1", "2", "3", "4", "5", "6", "7", "8")

BB_ALL = (1 << 64) - 1
BB_SQUARES = tuple(1 << square_index for square_index in range(64))
BB_FILES = tuple(0x0101010101010101 << file_index for file_index in range(8))
BB_RANKS = tuple(0xFF << (8 * rank_index) for rank_index in range(8))

BB_FILE_A = BB_FILES[0]
BB_FILE_H = BB_FILES[7]
BB_RANK_1 = BB_RANKS[0]
BB_RANK_2 = BB_RANKS[1]
BB_RANK_3 = BB_RANKS[2]
BB_RANK_4 = BB_RANKS[3]
BB_RANK_5 = BB_RANKS[4]
BB_RANK_6 = BB_RANKS[5]
BB_RANK_7 = BB_RANKS[6]
BB_RANK_8 = BB_RANKS[7]

BB_LIGHT_SQUARES = 0
BB_DARK_SQUARES = 0
for _square_index in range(64):
    if (_square_index + (_square_index >> 3)) % 2:
        BB_LIGHT_SQUARES |= BB_SQUARES[_square_index]
    else:
        BB_DARK_SQUARES |= BB_SQUARES[_square_index]
del _square_index

PROMOTION_SYMBOLS = "nbrq"


def square(file_index: int, rank_index: int) -> int:
    """Return the square index for a zero-based file and rank."""
    return rank_index * 8 + file_index


def square_file(square_index: int) -> int:
    """Return the zero-based file of a square."""
    return square_index & 7


def square_rank(square_index: int) -> int:
    """Return the zero-based rank of a square."""
    return square_index >> 3


def square_name(square_index: int) -> str:
    """Return the algebraic name (for example ``e4``) of a square."""
    return FILE_NAMES[square_index & 7] + RANK_NAMES[square_index >> 3]


def parse_square(name: str) -> int:
    """Parse an algebraic square name into a square index."""
    if not isinstance(name, str) or len(name) != 2:
        raise ValueError(f"expected a square name like 'e4', got {name!r}")
    try:
        file_index = FILE_NAMES.index(name[0].lower())
        rank_index = RANK_NAMES.index(name[1])
    except ValueError:
        raise ValueError(f"invalid square name: {name!r}") from None
    return rank_index * 8 + file_index


def bit_scan(bb: int) -> int:
    """Return the index of the least significant set bit."""
    return (bb & -bb).bit_length() - 1


def iter_bits(bb: int):
    """Yield the indices of every set bit in a bitboard."""
    while bb:
        yield (bb & -bb).bit_length() - 1
        bb &= bb - 1


class Piece:
    """A colored piece, matching the python-chess value semantics."""

    __slots__ = ("piece_type", "color")

    def __init__(self, piece_type: int, color: bool):
        if piece_type not in PIECE_TYPES:
            raise ValueError(f"invalid piece type: {piece_type!r}")
        if color not in (WHITE, BLACK):
            raise ValueError(f"invalid piece color: {color!r}")
        self.piece_type = piece_type
        self.color = color

    @classmethod
    def from_symbol(cls, symbol: str) -> "Piece":
        if len(symbol) != 1:
            raise ValueError(f"expected a single piece symbol, got {symbol!r}")
        lowered = symbol.lower()
        if lowered not in "pnbrqk":
            raise ValueError(f"invalid piece symbol: {symbol!r}")
        piece_type = "pnbrqk".index(lowered) + PAWN
        return cls(piece_type, WHITE if symbol.isupper() else BLACK)

    def symbol(self) -> str:
        symbol = PIECE_SYMBOLS[self.piece_type]
        return symbol if self.color == WHITE else symbol.lower()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Piece):
            return NotImplemented
        return self.piece_type == other.piece_type and self.color == other.color

    def __hash__(self) -> int:
        return hash((self.piece_type, self.color))

    def __repr__(self) -> str:
        return f"Piece.from_symbol({self.symbol()!r})"


class Move:
    """A move between two squares with an optional promotion piece type."""

    __slots__ = ("from_square", "to_square", "promotion")

    def __init__(self, from_square: int, to_square: int, promotion: int | None = None):
        if promotion is not None and promotion not in (KNIGHT, BISHOP, ROOK, QUEEN):
            raise ValueError(f"invalid promotion piece type: {promotion!r}")
        self.from_square = from_square
        self.to_square = to_square
        self.promotion = promotion

    @classmethod
    def from_uci(cls, uci: str) -> "Move":
        if not isinstance(uci, str):
            raise ValueError(f"expected a uci string, got {uci!r}")
        if len(uci) == 4:
            return cls(parse_square(uci[0:2]), parse_square(uci[2:4]))
        if len(uci) == 5:
            symbol = uci[4].lower()
            if symbol not in PROMOTION_SYMBOLS:
                raise ValueError(f"invalid promotion in uci string: {uci!r}")
            promotion = PROMOTION_SYMBOLS.index(symbol) + KNIGHT
            return cls(parse_square(uci[0:2]), parse_square(uci[2:4]), promotion)
        raise ValueError(f"expected a uci string of length 4 or 5, got {uci!r}")

    @classmethod
    def null(cls) -> "Move":
        return cls(0, 0)

    def uci(self) -> str:
        promotion = PROMOTION_SYMBOLS[self.promotion - KNIGHT] if self.promotion else ""
        return square_name(self.from_square) + square_name(self.to_square) + promotion

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Move):
            return NotImplemented
        return (
            self.from_square == other.from_square
            and self.to_square == other.to_square
            and self.promotion == other.promotion
        )

    def __hash__(self) -> int:
        return hash((self.from_square, self.to_square, self.promotion))

    def __str__(self) -> str:
        return self.uci()

    def __repr__(self) -> str:
        return f"Move.from_uci({self.uci()!r})"
