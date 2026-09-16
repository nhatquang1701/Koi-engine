#include <cstdint>
#include <iostream>
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

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"start-position perft", test_start_position_perft_counts},
        {"Kiwipete perft", test_kiwipete_perft_counts},
        {"endgame pin perft", test_endgame_perft_counts_cover_pins_and_en_passant_legality},
    };

    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
