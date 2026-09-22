"""Clean-room, standard-library-only chess toolkit for Koi's Python tooling.

The package mirrors the subset of the python-chess API that Koi's measurement
and NNUE tooling used before the dependency was removed.  Call sites use
``import koi_chess as chess``; no python-chess code is included or required.

Submodules are imported eagerly so that ``koi_chess.engine`` and
``koi_chess.pgn`` are available after a plain ``import koi_chess``.
"""

from __future__ import annotations

from .board import STARTING_FEN, Board, Outcome, Termination
from .core import (
    BB_ALL,
    BB_SQUARES,
    BISHOP,
    BLACK,
    FILE_NAMES,
    KING,
    KNIGHT,
    PAWN,
    PIECE_NAMES,
    PIECE_SYMBOLS,
    PIECE_TYPES,
    QUEEN,
    RANK_NAMES,
    ROOK,
    WHITE,
    Move,
    Piece,
    parse_square,
    square,
    square_file,
    square_name,
    square_rank,
)
from . import pgn  # noqa: E402  (imported after the core re-exports)

__all__ = [
    "BB_ALL",
    "BB_SQUARES",
    "BISHOP",
    "BLACK",
    "Board",
    "FILE_NAMES",
    "KING",
    "KNIGHT",
    "Move",
    "Outcome",
    "PAWN",
    "PIECE_NAMES",
    "PIECE_SYMBOLS",
    "PIECE_TYPES",
    "Piece",
    "QUEEN",
    "RANK_NAMES",
    "ROOK",
    "STARTING_FEN",
    "WHITE",
    "Termination",
    "parse_square",
    "pgn",
    "square",
    "square_file",
    "square_name",
    "square_rank",
]
