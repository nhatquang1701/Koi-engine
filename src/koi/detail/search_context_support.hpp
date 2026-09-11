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

[[nodiscard]] bool quiet_move_has_direct_forcing_target(
    const PositionFeatures& before, const MoveMetadata& metadata);

[[nodiscard]] bool quiet_move_has_king_ring_target(
    const PositionFeatures& before, const MoveMetadata& metadata);

[[nodiscard]] bool quiet_move_has_pawn_break_target(
    const MoveMetadata& metadata) noexcept;

[[nodiscard]] bool null_move_is_safe(const GameState& state,
                                     const PositionFeatures& features) noexcept;

[[nodiscard]] bool deep_quiet_check_candidate(const MoveMetadata& metadata) noexcept;

[[nodiscard]] bool narrow_deep_quiet_check_candidate(
    GameState& state, const MoveMetadata& metadata);

[[nodiscard]] bool root_move_exposes_immediate_check(
    const GameState& root, const MoveMetadata& metadata) noexcept;

} // namespace koi::detail
