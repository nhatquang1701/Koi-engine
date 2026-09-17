#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/static_exchange.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

koi::GameState require_state(std::string_view fen) {
    return koi::test::require_value(koi::GameState::from_fen(fen),
                                    "test FEN must construct a game state");
}

koi::Move require_move(std::string_view uci) {
    return koi::test::require_value(koi::Move::parse_uci(uci), "test move must parse");
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

void test_tactical_metadata_does_not_build_evaluation_features_for_exchange_scoring() {
    koi::GameState state = require_state("3rk3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1");
    const std::uint64_t misses_before = state.position_feature_cache_misses();
    koi::MoveMetadataList moves;
    (void)state.legal_tactical_moves_with_metadata(moves, true, true);
    require(state.position_feature_cache_misses() == misses_before,
            "tactical SEE must not build evaluation feature snapshots");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"winning capture", test_winning_capture_has_positive_exchange_gain},
        {"poisoned capture", test_poisoned_capture_has_negative_exchange_gain},
        {"non-capture", test_non_capture_has_zero_exchange_gain},
        {"quiet promotions", test_quiet_promotions_have_zero_exchange_gain},
        {"metadata exchange", test_metadata_exchange_path_matches_move_exchange_path},
        {"en passant capture", test_en_passant_capture_keeps_the_captured_pawn_value},
        {"capturing promotion", test_capturing_promotion_counts_capture_and_promotion_gain},
        {"promotion recapture", test_promotion_recapture_uses_the_promoted_piece_value},
        {"pinned recapturer", test_pinned_recapturer_does_not_reduce_exchange_gain},
        {"unsafe king recapture", test_unsafe_king_recapture_does_not_reduce_exchange_gain},
        {"tactical SEE without feature snapshots",
         test_tactical_metadata_does_not_build_evaluation_features_for_exchange_scoring},
    };
    return koi::test::run_tests(tests, argc, argv);
}
