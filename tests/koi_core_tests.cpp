#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/position.hpp"
#include "koi/move_chooser.hpp"

namespace {

using koi::Move;
using koi::MoveChooser;
using koi::Position;
using koi::RandomMoveChooser;

constexpr std::string_view kInitialFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

bool contains_uci_move(const Position& position, std::string_view expected) {
    for (const Move& move : position.legal_moves()) {
        if (move.uci() == expected) {
            return true;
        }
    }
    return false;
}

void test_default_position_has_initial_fen_and_twenty_moves() {
    const Position position;

    require(position.fen() == kInitialFen, "default position must use the standard initial FEN");
    require(position.legal_moves().size() == 20, "default position must have exactly 20 legal moves");
}

void test_legal_uci_sequence_updates_position() {
    Position position;

    position.apply_uci("e2e4");
    require(position.fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            "e2e4 must update the position");

    position.apply_uci("e7e5");
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2",
            "e7e5 must update the position");

    position.apply_uci("g1f3");
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
            "g1f3 must update the position");
}

void test_special_move_positions_accept_valid_uci_moves() {
    Position castling("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    require(contains_uci_move(castling, "e1g1"), "castling position must expose kingside castling");
    castling.apply_uci("e1g1");

    Position en_passant("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    require(contains_uci_move(en_passant, "d5e6"), "en passant position must expose the valid capture");
    en_passant.apply_uci("d5e6");

    Position promotion("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    require(contains_uci_move(promotion, "a7a8q"), "promotion position must expose a queen promotion");
    promotion.apply_uci("a7a8q");
}

void test_checkmate_and_stalemate_have_no_legal_moves() {
    const Position checkmate("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    require(checkmate.legal_moves().empty(), "checkmate position must have no legal moves");

    const Position stalemate("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    require(stalemate.legal_moves().empty(), "stalemate position must have no legal moves");
}

void test_random_chooser_returns_a_legal_move() {
    const Position position;
    RandomMoveChooser chooser(1234);

    const Move selected = chooser.choose(position);
    require(contains_uci_move(position, selected.uci()), "random chooser must return a legal move");
}

void test_seeded_choosers_are_repeatable() {
    const Position position;
    RandomMoveChooser first(5678);
    RandomMoveChooser second(5678);

    for (int i = 0; i < 12; ++i) {
        require(first.choose(position).uci() == second.choose(position).uci(),
                "choosers with the same seed must produce the same sequence");
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"default position", test_default_position_has_initial_fen_and_twenty_moves},
        {"legal UCI sequence", test_legal_uci_sequence_updates_position},
        {"special moves", test_special_move_positions_accept_valid_uci_moves},
        {"checkmate and stalemate", test_checkmate_and_stalemate_have_no_legal_moves},
        {"random chooser legality", test_random_chooser_returns_a_legal_move},
        {"seeded chooser repeatability", test_seeded_choosers_are_repeatable},
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
