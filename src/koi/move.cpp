#include "koi/move.hpp"

namespace koi {

Move::Move(chess::Move move) noexcept : move_(move) {}

Move Move::no_move() noexcept {
    return Move(chess::Move(chess::Move::NO_MOVE));
}

std::string Move::uci() const {
    if (move_.move() == chess::Move::NO_MOVE) {
        return "0000";
    }

    return chess::uci::moveToUci(move_);
}

chess::Move Move::native() const noexcept {
    return move_;
}

} // namespace koi
