#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"

#ifdef CHESS_HPP
#error "Public Koi rules headers must not include chess.hpp"
#endif

namespace {

std::atomic_bool fail_next_allocation = false;

using koi::GameState;
using koi::Move;
using koi::Promotion;
using koi::Square;

constexpr std::string_view kInitialFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Move require_move(std::string_view uci) {
    const auto move = Move::parse_uci(uci);
    require(move.has_value(), "test fixture must be valid coordinate UCI");
    return *move;
}

void test_move_uses_koi_coordinates_and_formats_uci() {
    const auto from = Square::parse("e2");
    const auto to = Square::parse("e4");
    require(from.has_value() && to.has_value(), "coordinate fixtures must parse");

    const Move move(*from, *to);
    require(move.from() == *from && move.to() == *to, "move must retain Koi square coordinates");
    require(move.promotion() == Promotion::none, "ordinary move must have no promotion");
    require(move.uci() == "e2e4", "move must format coordinate UCI without a library move");
    require(Move::parse_uci("e7e8q") == Move(*Square::parse("e7"), *Square::parse("e8"), Promotion::queen),
            "move parser must retain promotion in Koi-owned data");
}

void test_fen_constructs_a_game_state() {
    const auto state = GameState::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(state.has_value(), "valid FEN must construct a GameState");
    require(state->fen() == "4k3/8/8/8/8/8/8/4K3 w - - 0 1", "GameState must preserve valid FEN");
}

void test_game_state_exposes_legal_special_moves() {
    const auto castling = GameState::from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    const auto en_passant = GameState::from_fen("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    const auto promotion = GameState::from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

    require(castling && castling->is_legal(require_move("e1g1")), "castling must remain a legal Koi move");
    require(en_passant && en_passant->is_legal(require_move("d5e6")),
            "en passant must remain a legal Koi move");
    require(promotion && promotion->is_legal(require_move("a7a8q")),
            "promotion must remain a legal Koi move");
}

void test_make_and_unmake_restore_fen_and_key() {
    GameState state = GameState::startpos();
    const std::string original_fen = state.fen();
    const std::uint64_t original_key = state.position_key();

    require(state.make_move(require_move("e2e4")), "legal move must be made");
    require(state.unmake_move(), "made move must be unmade");
    require(state.fen() == original_fen, "unmake must restore the original FEN");
    require(state.position_key() == original_key, "unmake must restore the original position key");
}

void test_failed_history_recording_leaves_state_unchanged() {
    GameState state = GameState::startpos();
    const std::string original_fen = state.fen();
    const std::uint64_t original_key = state.position_key();
    const Move move = require_move("e2e4");

    fail_next_allocation = true;
    const bool made_move = state.make_move(move);
    fail_next_allocation = false;

    require(!made_move, "history-allocation failure must make the move fail");
    require(state.fen() == original_fen, "failed move must preserve FEN");
    require(state.position_key() == original_key, "failed move must preserve the position key");
    require(!state.unmake_move(), "failed move must not add an undo entry");
}

void test_start_position_has_twenty_legal_moves() {
    const GameState state = GameState::startpos();
    require(state.fen() == kInitialFen, "startpos must use the standard initial FEN");
    require(state.legal_moves().size() == 20, "startpos must expose exactly twenty legal moves");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

void* operator new(std::size_t size) {
    if (fail_next_allocation.exchange(false)) {
        throw std::bad_alloc();
    }

    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    const std::vector<TestCase> tests{
        {"Koi-owned move coordinates", test_move_uses_koi_coordinates_and_formats_uci},
        {"FEN construction", test_fen_constructs_a_game_state},
        {"legal special moves", test_game_state_exposes_legal_special_moves},
        {"make/unmake restoration", test_make_and_unmake_restore_fen_and_key},
        {"transactional history failure", test_failed_history_recording_leaves_state_unchanged},
        {"start-position move count", test_start_position_has_twenty_legal_moves},
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
