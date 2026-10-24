#include <cstdint>
#include <iostream>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/perft.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

void test_start_position_perft_counts() {
    koi::GameState state = koi::GameState::startpos();

    require(koi::perft(state, 1) == 20, "start position depth 1 must have 20 nodes");
    require(koi::perft(state, 2) == 400, "start position depth 2 must have 400 nodes");
    require(koi::perft(state, 3) == 8902, "start position depth 3 must have 8902 nodes");
}

void test_kiwipete_perft_counts() {
    constexpr std::string_view kKiwipete =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    const auto parsed = koi::GameState::from_fen(kKiwipete);
    require(parsed.has_value(), "Kiwipete fixture must be a legal position");
    koi::GameState state = *parsed;

    require(koi::perft(state, 1) == 48, "Kiwipete depth 1 must have 48 nodes");
    require(koi::perft(state, 2) == 2039, "Kiwipete depth 2 must have 2039 nodes");
    require(koi::perft(state, 3) == 97862, "Kiwipete depth 3 must have 97862 nodes");
}

void test_endgame_perft_counts_cover_pins_and_en_passant_legality() {
    constexpr std::string_view kPositionThree =
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1";
    const auto parsed = koi::GameState::from_fen(kPositionThree);
    require(parsed.has_value(), "position three fixture must be a legal position");
    koi::GameState state = *parsed;

    require(koi::perft(state, 1) == 14, "position three depth 1 must have 14 nodes");
    require(koi::perft(state, 2) == 191, "position three depth 2 must have 191 nodes");
    require(koi::perft(state, 3) == 2812, "position three depth 3 must have 2812 nodes");
}

void test_promotion_and_castling_perft_fixtures() {
    // CPW position 4: promotion / check-evasion rich.
    const auto promotion = koi::GameState::from_fen(
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    require(promotion.has_value(), "the promotion perft fixture must parse");
    koi::GameState promotion_state = *promotion;
    require(koi::perft(promotion_state, 1) == 6,
            "promotion fixture depth 1 must have 6 nodes");
    require(koi::perft(promotion_state, 2) == 264,
            "promotion fixture depth 2 must have 264 nodes");
    require(koi::perft(promotion_state, 3) == 9467,
            "promotion fixture depth 3 must have 9467 nodes");

    // CPW position 5: castling rights, en passant, and tactical legality.
    const auto castling = koi::GameState::from_fen(
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    require(castling.has_value(), "the castling perft fixture must parse");
    koi::GameState castling_state = *castling;
    require(koi::perft(castling_state, 1) == 44,
            "castling fixture depth 1 must have 44 nodes");
    require(koi::perft(castling_state, 2) == 1486,
            "castling fixture depth 2 must have 1486 nodes");
    require(koi::perft(castling_state, 3) == 62379,
            "castling fixture depth 3 must have 62379 nodes");
}

void test_interruptible_perft_matches_standard_count_and_restores_position() {
    koi::GameState state = koi::GameState::startpos();
    const std::string root_fen = state.fen();

    const koi::PerftResult result = koi::perft_interruptible(state, 3, {});

    require(!result.cancelled && result.nodes == 8902,
            "completed interruptible perft must match the native legal-move count");
    require(state.fen() == root_fen,
            "completed interruptible perft must restore the root position");
}

void test_interruptible_perft_stops_before_generating_moves() {
    koi::GameState state = koi::GameState::startpos();
    const std::string root_fen = state.fen();
    std::stop_source stop_source;
    stop_source.request_stop();

    const koi::PerftResult result =
        koi::perft_interruptible(state, 10, stop_source.get_token());

    require(result.cancelled && result.nodes == 0,
            "a pre-requested stop must abort perft without counting nodes");
    require(state.fen() == root_fen,
            "cancelling interruptible perft must preserve the root position");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"start-position perft", test_start_position_perft_counts},
        {"Kiwipete perft", test_kiwipete_perft_counts},
        {"endgame pin perft", test_endgame_perft_counts_cover_pins_and_en_passant_legality},
        {"promotion and castling perft", test_promotion_and_castling_perft_fixtures},
        {"interruptible perft count and restore", test_interruptible_perft_matches_standard_count_and_restores_position},
        {"interruptible perft stop", test_interruptible_perft_stops_before_generating_moves},
    };

    return koi::test::run_tests(tests, argc, argv);
}
