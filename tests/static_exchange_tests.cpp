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

void test_quiet_promotions_have_zero_exchange_gain() {
    const koi::GameState state = require_state("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    for (const std::string_view promotion : {"q", "r", "b", "n"}) {
        const std::string uci = "a7a8" + std::string(promotion);
        require(koi::detail::static_exchange_gain(state, require_move(uci)) == 0,
                "a non-capturing promotion must not have exchange gain");
    }
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

void test_en_passant_capture_keeps_the_captured_pawn_value() {
    const koi::GameState state = require_state("4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 2");
    require(koi::detail::static_exchange_gain(state, require_move("d5e6")) == 100,
            "an en-passant capture must count the pawn removed behind its destination square");
}

void test_capturing_promotion_counts_capture_and_promotion_gain() {
    const koi::GameState state = require_state("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    for (const auto& [promotion, expected] : {std::pair{"q", 1300}, std::pair{"r", 900},
                                              std::pair{"b", 730}, std::pair{"n", 720}}) {
        const std::string uci = "a7b8" + std::string(promotion);
        require(koi::detail::static_exchange_gain(state, require_move(uci)) == expected,
                "a capturing promotion must count the captured rook and its selected promotion gain");
    }
}

void test_promotion_recapture_uses_the_promoted_piece_value() {
    const koi::GameState state = require_state("4k3/8/8/8/8/1R6/2p5/1n2K3 w - - 0 1");
    require(koi::detail::static_exchange_gain(state, require_move("b3b1")) == -980,
            "a back-rank pawn recapture must include its queen promotion gain");
}

void test_pinned_recapturer_does_not_reduce_exchange_gain() {
    const koi::GameState state = require_state("4k3/4n3/8/3p4/2Q5/8/8/4R1K1 w - - 0 1");
    require(koi::detail::static_exchange_gain(state, require_move("c4d5")) == 100,
            "a pinned knight must not be selected as a legal recapturer");
}

void test_unsafe_king_recapture_does_not_reduce_exchange_gain() {
    const koi::GameState state = require_state("8/8/3k4/3p4/4Q3/8/8/3R2K1 w - - 0 1");
    require(koi::detail::static_exchange_gain(state, require_move("e4d5")) == 100,
            "a king may not recapture onto a square protected by the opposing rook");
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
        test_quiet_promotions_have_zero_exchange_gain();
        std::cout << "PASS quiet promotions\n";
        test_metadata_exchange_path_matches_move_exchange_path();
        std::cout << "PASS metadata exchange\n";
        test_en_passant_capture_keeps_the_captured_pawn_value();
        std::cout << "PASS en passant capture\n";
        test_capturing_promotion_counts_capture_and_promotion_gain();
        std::cout << "PASS capturing promotion\n";
        test_promotion_recapture_uses_the_promoted_piece_value();
        std::cout << "PASS promotion recapture\n";
        test_pinned_recapturer_does_not_reduce_exchange_gain();
        std::cout << "PASS pinned recapturer\n";
        test_unsafe_king_recapture_does_not_reduce_exchange_gain();
        std::cout << "PASS unsafe king recapture\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL static exchange: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
