#include "koi/perft.hpp"

namespace koi {

std::uint64_t perft(GameState& state, int depth) {
    if (depth <= 0) {
        return 1;
    }

    std::uint64_t nodes = 0;
    for (const Move& move : state.legal_moves()) {
        if (!state.make_move(move)) {
            continue;
        }
        nodes += perft(state, depth - 1);
        state.unmake_move();
    }
    return nodes;
}

PerftResult perft_interruptible(GameState& state, const int depth,
                                const std::stop_token stop_token) {
    if (stop_token.stop_requested()) {
        return {.nodes = 0, .cancelled = true};
    }
    if (depth <= 0) {
        return {.nodes = 1, .cancelled = false};
    }

    std::uint64_t nodes = 0;
    for (const Move& move : state.legal_moves()) {
        if (stop_token.stop_requested()) {
            return {.nodes = nodes, .cancelled = true};
        }
        if (!state.make_move(move)) {
            continue;
        }
        const PerftResult child = perft_interruptible(state, depth - 1, stop_token);
        state.unmake_move();
        nodes += child.nodes;
        if (child.cancelled) {
            return {.nodes = nodes, .cancelled = true};
        }
    }
    return {.nodes = nodes, .cancelled = false};
}

} // namespace koi
