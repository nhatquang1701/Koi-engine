#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/position.hpp"
#include "koi/move_chooser.hpp"

namespace {

using koi::Move;
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
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            "e7e5 must update the position");

    position.apply_uci("g1f3");
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
            "g1f3 must update the position");
}

void test_malformed_fens_are_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    for (std::string_view fen : {
             "8",
             "8/8/8/8/8/8/8/8 w - - 0 1",
             "8/8/8/8/8/8/8/K6k x - - 0 1",
             "8/8/8/8/8/8/8/K6k w - e4 0 1",
             "8/8/8/8/8/8/8/K6k w - - -1 1",
         }) {
        require(!position.set_fen(fen), "malformed or kingless FEN must be rejected");
        require(position.fen() == original, "rejected FEN must leave the position unchanged");
    }
}

void test_adjacent_kings_are_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.set_fen("8/8/8/8/8/8/4k3/4K3 w - - 0 1"),
            "FEN with adjacent kings must be rejected");
    require(position.fen() == original, "rejected adjacent-kings FEN must leave the position unchanged");
}

void test_invalid_uci_is_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.apply_uci("e2e5"), "illegal UCI move must be rejected");
    require(position.fen() == original, "rejected UCI move must leave the position unchanged");
}

void test_position_fen_matches_the_underlying_board() {
    Position position;
    require(position.apply_uci("e2e4"), "e2e4 must be legal");
    require(position.apply_uci("e7e5"), "e7e5 must be legal");

    require(position.fen() == position.board().getFen(),
            "adapter FEN must match the underlying board FEN");
}

void test_special_move_positions_accept_valid_uci_moves() {
    Position castling("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    require(contains_uci_move(castling, "e1g1"), "castling position must expose kingside castling");
    castling.apply_uci("e1g1");
    require(castling.fen() == "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1",
            "castling must move the rook and remove white castling rights");

    Position en_passant("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    require(contains_uci_move(en_passant, "d5e6"), "en passant position must expose the valid capture");
    en_passant.apply_uci("d5e6");
    require(en_passant.fen() == "rnbqkbnr/pppp1ppp/4P3/8/8/8/PPP1PPPP/RNBQKBNR b KQkq - 0 2",
            "en passant must remove the captured pawn and clear the en passant square");

    Position promotion("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    require(contains_uci_move(promotion, "a7a8q"), "promotion position must expose a queen promotion");
    promotion.apply_uci("a7a8q");
    require(promotion.fen() == "Q3k3/8/8/8/8/8/8/4K3 b - - 0 1",
            "promotion must replace the pawn with a queen and preserve castling rights as none");
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
        {"malformed FEN rejection", test_malformed_fens_are_rejected_transactionally},
        {"adjacent king rejection", test_adjacent_kings_are_rejected_transactionally},
        {"invalid UCI rejection", test_invalid_uci_is_rejected_transactionally},
        {"adapter FEN matches board", test_position_fen_matches_the_underlying_board},
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
