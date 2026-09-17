#pragma once

#include <cstdint>

// Precomputed attack targets for the native board.  The native position keeps
// mailbox squares and bitboards in sync, so every attack query can be answered
// with table lookups instead of scanning the board.  Sliding attacks are built
// from per-direction ray tables with the classical "nearest blocker" trick,
// which keeps the tables small (no magic numbers) while still being O(1).

namespace koi::detail {

// Direction order used by the ray tables.  Steps are square-index deltas.
// Positive steps move towards larger square indices (N, NE, E, NW); negative
// steps move towards smaller ones (SE, S, SW, W).
inline constexpr int kAttackDirections = 8;

// Squares a knight on `square` attacks.
[[nodiscard]] std::uint64_t knight_attacks(int square) noexcept;

// Squares a king on `square` attacks.
[[nodiscard]] std::uint64_t king_attacks(int square) noexcept;

// Squares a pawn of the given colour standing on `square` attacks.
[[nodiscard]] std::uint64_t pawn_attacks(int square, bool white) noexcept;

// Squares from which a pawn of the given colour would attack `target`.
[[nodiscard]] std::uint64_t pawn_attackers_of(int target, bool white_pawns) noexcept;

// Squares a sliding piece on `square` attacks with the given occupancy.
[[nodiscard]] std::uint64_t bishop_attacks(int square, std::uint64_t occupied) noexcept;
[[nodiscard]] std::uint64_t rook_attacks(int square, std::uint64_t occupied) noexcept;
[[nodiscard]] std::uint64_t queen_attacks(int square, std::uint64_t occupied) noexcept;

// Empty-board ray from `square` in `direction`, or 0 when off the board.
[[nodiscard]] std::uint64_t ray_attacks(int square, int direction) noexcept;

// Nearest occupied square on the ray from `square` in `direction`, or -1 when
// the ray is empty or leaves the board.
[[nodiscard]] int nearest_blocker(int square, int direction, std::uint64_t occupied) noexcept;

} // namespace koi::detail
