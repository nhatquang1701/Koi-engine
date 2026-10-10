#pragma once

#include <cstdint>

#include "koi/game_state.hpp"

namespace koi::detail {

[[nodiscard]] int piece_value(PieceType type) noexcept;

[[nodiscard]] bool piece_attacks_square(const PositionFeatures& features,
                                        std::uint8_t source,
                                        std::uint8_t target) noexcept;

[[nodiscard]] bool quiet_move_is_forcing(const PositionFeatures& before,
                                         const PositionFeatures& after_features,
                                         const MoveMetadata& metadata);

// Compact after-move view for the quiet forcing probe.  It carries only the
// child fields quiet_move_is_forcing() consumes, built directly from the
// native position bitboards, so LMR candidates that are pruned before
// evaluation do not pay for a full PositionFeatures snapshot.
struct QuietForcingAfterView {
    // Post-move occupancy from the maintained bitboards.  Zero marks an
    // unbuilt view (or a native position with no pieces), which makes the
    // compact probe fall back to the full-features decision.
    std::uint64_t occupied = 0;
    std::uint64_t enemy_pieces = 0;
    std::uint64_t enemy_pawns = 0;
    // Union of the mover's piece attacks with post-move occupancy; equals
    // PositionFeatures::attacked_squares[own].
    std::uint64_t own_attacked_squares = 0;
    // Equals PositionFeatures::king_zone_attacks[enemy].
    std::uint8_t enemy_king_zone_attacks = 0;
};

// Builds the compact view for the child position reached by a quiet move of
// `mover`.  Uses the same attack tables and post-move occupancy as the full
// feature extraction, so every field matches the corresponding
// PositionFeatures field bit for bit.
void build_quiet_forcing_after_view(const GameState& after, Color mover,
                                    QuietForcingAfterView& out) noexcept;

// Compact twin of quiet_move_is_forcing(before, after_features, metadata):
// decides from a prebuilt view instead of a full child snapshot.  The
// metadata guard, king-zone comparison, attacked-piece scan, newly-attacked
// scan, and passed-pawn branch run in the same order and visit the same
// squares, so the returned bool is identical.  A view with occupied == 0
// falls back to the full-features path, preserving the board-scanning
// behavior for hand-built fixtures.
[[nodiscard]] bool quiet_move_is_forcing(const PositionFeatures& before,
                                         const GameState& after_state,
                                         const QuietForcingAfterView& after_view,
                                         const MoveMetadata& metadata);

[[nodiscard]] bool quiet_move_has_direct_forcing_target(
    const PositionFeatures& before, const MoveMetadata& metadata);

[[nodiscard]] bool quiet_move_has_king_ring_target(
    const PositionFeatures& before, const MoveMetadata& metadata);

[[nodiscard]] bool quiet_move_has_pawn_break_target(
    const MoveMetadata& metadata) noexcept;

// `phase` is GameState::game_phase(); passing the scalar keeps this gate from
// forcing a full feature snapshot.
[[nodiscard]] bool null_move_is_safe(const GameState& state, int phase) noexcept;

[[nodiscard]] bool deep_quiet_check_candidate(const MoveMetadata& metadata) noexcept;

[[nodiscard]] bool narrow_deep_quiet_check_candidate(
    GameState& state, const MoveMetadata& metadata);

[[nodiscard]] bool root_move_exposes_immediate_check(
    const GameState& root, const MoveMetadata& metadata) noexcept;

} // namespace koi::detail
