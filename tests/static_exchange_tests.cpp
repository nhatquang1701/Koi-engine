#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "koi/detail/static_exchange.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "SEE fixture must construct a game state");
    return *state;
}

koi::Move require_move(std::string_view uci) {
    const auto move = koi::Move::parse_uci(uci);
    require(move.has_value(), "SEE fixture move must parse");
    return *move;
}

void test_winning_capture_has_positive_exchange_gain() {
    const koi::GameState state = require_state("4k3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1");
    const int gain = koi::detail::static_exchange_gain(state, require_move("e4d5"));
    require(gain > 0, "a free pawn capture must have positive exchange gain");
}

void test_poisoned_capture_has_negative_exchange_gain() {
    const koi::GameState state = require_state("3rk3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1");
    const int gain = koi::detail::static_exchange_gain(state, require_move("e4d5"));
    require(gain < -500, "a queen capture that loses to a rook must have negative exchange gain");
}

void test_non_capture_has_zero_exchange_gain() {
    const koi::GameState state = require_state("4k3/8/8/8/4Q3/8/8/4K3 w - - 0 1");
    require(koi::detail::static_exchange_gain(state, require_move("e4e5")) == 0,
            "non-captures must not have exchange gain");
}

void test_metadata_exchange_path_matches_move_exchange_path() {
    const koi::GameState state = require_state("3rk3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1");
    const koi::Move move = require_move("e4d5");
    const auto metadata = state.describe_move(move);
    require(metadata.has_value() && metadata->is_capture(),
            "metadata exchange fixture must describe a capture");
    require(koi::detail::static_exchange_gain(state, *metadata) ==
                koi::detail::static_exchange_gain(state, move),
            "metadata exchange evaluation must match the move compatibility path");
}

} // namespace

int main() {
    try {
        test_winning_capture_has_positive_exchange_gain();
        std::cout << "PASS winning capture\n";
        test_poisoned_capture_has_negative_exchange_gain();
        std::cout << "PASS poisoned capture\n";
        test_non_capture_has_zero_exchange_gain();
        std::cout << "PASS non-capture\n";
        test_metadata_exchange_path_matches_move_exchange_path();
        std::cout << "PASS metadata exchange\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL static exchange: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
