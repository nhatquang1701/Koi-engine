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

} // namespace koi
