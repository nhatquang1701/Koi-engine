#pragma once

#include <cstdint>
#include <stop_token>

#include "koi/game_state.hpp"

namespace koi {

struct PerftResult {
    std::uint64_t nodes = 0;
    bool cancelled = false;
};

[[nodiscard]] std::uint64_t perft(GameState& state, int depth);
[[nodiscard]] PerftResult perft_interruptible(GameState& state, int depth,
                                               std::stop_token stop_token);

} // namespace koi
