#include "koi/position.hpp"

#include <stdexcept>
#include <utility>

namespace koi {

Position::Position() = default;

Position::Position(std::string_view fen) {
    if (!set_fen(fen)) {
        throw std::invalid_argument("invalid FEN");
    }
}

std::string Position::fen() const {
    return state_.fen();
}

std::vector<Move> Position::legal_moves() const {
    return state_.legal_moves();
}

bool Position::apply_uci(std::string_view uci) {
    const auto move = Move::parse_uci(uci);
    return move && state_.make_move(*move);
}

bool Position::set_fen(std::string_view fen) {
    const auto candidate = GameState::from_fen(fen);
    if (!candidate) {
        return false;
    }
    state_ = *candidate;
    return true;
}

} // namespace koi
