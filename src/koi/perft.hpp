#pragma once

#include <cstdint>

#include "koi/game_state.hpp"

namespace koi {

[[nodiscard]] std::uint64_t perft(GameState& state, int depth);

} // namespace koi
