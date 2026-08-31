#include "koi/detail/static_exchange.hpp"

namespace koi::detail {
int static_exchange_gain(const GameState& state, const MoveMetadata& initial) noexcept {
    return state.direct_static_exchange_gain(initial);
}

int static_exchange_gain(const GameState& state, Move move) noexcept {
    const auto metadata = state.describe_move(move);
    return metadata.has_value() ? static_exchange_gain(state, *metadata) : 0;
}

} // namespace koi::detail
