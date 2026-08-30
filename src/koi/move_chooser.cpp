#include "koi/move_chooser.hpp"

#include <chrono>

#include "koi/position.hpp"

namespace koi {

namespace {

std::uint32_t runtime_seed() {
    std::random_device device;
    return device() ^ static_cast<std::uint32_t>(
                          std::chrono::steady_clock::now().time_since_epoch().count());
}

} // namespace

RandomMoveChooser::RandomMoveChooser(std::uint32_t seed)
    : engine_(seed == 0 ? runtime_seed() : seed) {}

Move RandomMoveChooser::choose(const Position& position) {
    const std::vector<Move> moves = position.legal_moves();
    if (moves.empty()) {
        return Move::no_move();
    }

    std::uniform_int_distribution<std::size_t> distribution(0, moves.size() - 1);
    return moves[distribution(engine_)];
}

} // namespace koi
