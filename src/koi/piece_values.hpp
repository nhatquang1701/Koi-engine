#pragma once

#include "koi/move.hpp"

namespace koi {

// Single source for the material values used by exchange evaluation, move
// ordering, opening-book scoring, and the classical evaluator's material term.
// These numbers used to be duplicated in four translation units; keeping one
// table means a tuning change cannot silently desynchronize them.
inline constexpr int kPawnMaterialValue = 100;
inline constexpr int kKnightMaterialValue = 320;
inline constexpr int kBishopMaterialValue = 330;
inline constexpr int kRookMaterialValue = 500;
inline constexpr int kQueenMaterialValue = 900;
// Kings are never captured, so this value only matters for material counting
// and for callers that deliberately reuse the table for king-safety math.
inline constexpr int kKingMaterialValue = 20'000;

[[nodiscard]] constexpr int piece_material_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return kPawnMaterialValue;
    case PieceType::knight:
        return kKnightMaterialValue;
    case PieceType::bishop:
        return kBishopMaterialValue;
    case PieceType::rook:
        return kRookMaterialValue;
    case PieceType::queen:
        return kQueenMaterialValue;
    case PieceType::king:
        return kKingMaterialValue;
    case PieceType::none:
        return 0;
    }
    return 0;
}

} // namespace koi
