"""Precomputed attack tables and sliding-piece attack generation.

Sliding attacks use one precomputed ray per direction and the classic
nearest-blocker trick, which keeps the encoder hot path close to the
performance of table-driven implementations.
"""

from __future__ import annotations

from .core import BISHOP, KING, KNIGHT, PAWN, QUEEN, ROOK, WHITE, BLACK

# Ray order: N, S, E, W (rook rays) then NE, NW, SE, SW (bishop rays).
_DIRECTIONS = (
    (0, 1),
    (0, -1),
    (1, 0),
    (-1, 0),
    (1, 1),
    (-1, 1),
    (1, -1),
    (-1, -1),
)

ROOK_RAYS = (0, 1, 2, 3)
BISHOP_RAYS = (4, 5, 6, 7)


def _build_rays():
    rays = [[0] * 64 for _ in range(8)]
    positive = []
    for index, (file_delta, rank_delta) in enumerate(_DIRECTIONS):
        positive.append(file_delta + rank_delta * 8 > 0)
        for square in range(64):
            file_index = square & 7
            rank_index = square >> 3
            bb = 0
            file_cursor = file_index + file_delta
            rank_cursor = rank_index + rank_delta
            while 0 <= file_cursor < 8 and 0 <= rank_cursor < 8:
                bb |= 1 << (rank_cursor * 8 + file_cursor)
                file_cursor += file_delta
                rank_cursor += rank_delta
            rays[index][square] = bb
    return rays, positive


_RAYS, _POSITIVE = _build_rays()


def sliding_attacks(square: int, occupied: int, ray_indices) -> int:
    """Return the attacks of a sliding piece along the given ray group."""
    attacks = 0
    for index in ray_indices:
        ray = _RAYS[index][square]
        blockers = ray & occupied
        if blockers:
            if _POSITIVE[index]:
                first = blockers & -blockers
                attacks |= (ray & (first - 1)) | first
            else:
                first = 1 << (blockers.bit_length() - 1)
                attacks |= (ray & ~((first << 1) - 1)) | first
        else:
            attacks |= ray
    return attacks


def _build_step_tables():
    knights = [0] * 64
    kings = [0] * 64
    pawns = {WHITE: [0] * 64, BLACK: [0] * 64}
    knight_deltas = (
        (1, 2),
        (2, 1),
        (2, -1),
        (1, -2),
        (-1, -2),
        (-2, -1),
        (-2, 1),
        (-1, 2),
    )
    king_deltas = (
        (0, 1),
        (1, 0),
        (0, -1),
        (-1, 0),
        (1, 1),
        (1, -1),
        (-1, 1),
        (-1, -1),
    )
    for square in range(64):
        file_index = square & 7
        rank_index = square >> 3
        for file_delta, rank_delta in knight_deltas:
            file_cursor = file_index + file_delta
            rank_cursor = rank_index + rank_delta
            if 0 <= file_cursor < 8 and 0 <= rank_cursor < 8:
                knights[square] |= 1 << (rank_cursor * 8 + file_cursor)
        for file_delta, rank_delta in king_deltas:
            file_cursor = file_index + file_delta
            rank_cursor = rank_index + rank_delta
            if 0 <= file_cursor < 8 and 0 <= rank_cursor < 8:
                kings[square] |= 1 << (rank_cursor * 8 + file_cursor)
        for file_delta in (-1, 1):
            file_cursor = file_index + file_delta
            if 0 <= file_cursor < 8:
                rank_cursor = rank_index + 1
                if rank_cursor < 8:
                    pawns[WHITE][square] |= 1 << (rank_cursor * 8 + file_cursor)
                rank_cursor = rank_index - 1
                if rank_cursor >= 0:
                    pawns[BLACK][square] |= 1 << (rank_cursor * 8 + file_cursor)
    return knights, kings, pawns


KNIGHT_ATTACKS, KING_ATTACKS, PAWN_ATTACKS = _build_step_tables()


def attacks_from(piece_type: int, color: bool, square: int, occupied: int) -> int:
    """Return the attack set of a piece type on a square for a position."""
    if piece_type == PAWN:
        return PAWN_ATTACKS[color][square]
    if piece_type == KNIGHT:
        return KNIGHT_ATTACKS[square]
    if piece_type == KING:
        return KING_ATTACKS[square]
    if piece_type == BISHOP:
        return sliding_attacks(square, occupied, BISHOP_RAYS)
    if piece_type == ROOK:
        return sliding_attacks(square, occupied, ROOK_RAYS)
    if piece_type == QUEEN:
        return sliding_attacks(square, occupied, BISHOP_RAYS) | sliding_attacks(
            square, occupied, ROOK_RAYS
        )
    raise ValueError(f"invalid piece type: {piece_type!r}")
