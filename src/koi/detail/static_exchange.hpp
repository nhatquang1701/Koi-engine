#pragma once

#include "koi/game_state.hpp"

namespace koi::detail {

// Returns the conservative material gain of a legal capture sequence on the
// destination square, from the moving side's perspective. Non-captures return
// zero.
[[nodiscard]] int static_exchange_gain(const GameState&, Move) noexcept;
[[nodiscard]] int static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;

} // namespace koi::detail
