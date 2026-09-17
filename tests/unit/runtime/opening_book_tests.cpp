#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/opening_book.hpp"

#include "koi_test_support.hpp"
#include "polyglot_book_support.hpp"

#ifdef CHESS_HPP
#error "Public Koi opening-book headers must not include chess.hpp"
#endif

namespace {

using koi::GameState;
using koi::Move;
using koi::OpeningBook;
using koi::test::book::BookRecord;
using koi::test::book::polyglot_move;
using koi::test::book::write_book;
using koi::test::require;

Move require_move(std::string_view uci) {
    return koi::test::require_value(Move::parse_uci(uci), "test move must parse");
}

GameState require_state(std::string_view fen) {
    return koi::test::require_value(GameState::from_fen(fen),
                                    "test FEN must construct a game state");
}

void test_polyglot_keys_match_reference_positions() {
    GameState start = GameState::startpos();
    require(start.polyglot_key() == 0x463b96181691fc9cULL,
            "the starting position must have the published Polyglot key");
    require(start.fullmove_number() == 1, "the initial fullmove number must be one");

    require(start.make_move(require_move("e2e4")), "e2e4 must be legal in the initial position");
    require(start.polyglot_key() == 0x823c9b50fd114196ULL,
            "the post-e4 position must have the reference Polyglot key");
    require(start.fullmove_number() == 1, "a white move must not advance the fullmove number");

    const GameState castling = require_state("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    require(castling.polyglot_key() == 0xfda239cc692a6053ULL,
            "castling rights must contribute the reference Polyglot values");

    const GameState en_passant = require_state(
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    const GameState no_en_passant = require_state(
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2");
    require(en_passant.polyglot_key() == 0x849991dfa8617835ULL,
            "a capturable en-passant file must contribute to the Polyglot key");
    require(no_en_passant.polyglot_key() == 0x4ba8d401a2bc3abcULL,
            "an absent en-passant square must not contribute to the Polyglot key");
}

void test_book_decodes_castling_en_passant_and_promotions() {
    koi::test::TempDirectory files;
    const auto book_path = files.path() / "special.bin";
    const GameState castling = require_state("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    const GameState en_passant = require_state(
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    const GameState promotion = require_state("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    require(promotion.polyglot_key() == 0xe7783b5a70da7685ULL,
            "promotion fixture must retain its reference Polyglot key");

    write_book(book_path, {
        {castling.polyglot_key(), polyglot_move("e1", "h1"), 1, 7},
        {en_passant.polyglot_key(), polyglot_move("d5", "e6"), 1, 8},
        {promotion.polyglot_key(), polyglot_move("a7", "a8", 4), 1, 9},
    });

    OpeningBook book(files.path());
    book.set_file(book_path);
    const auto castle_choice = book.choose(castling, 0, true, 16, 1);
    const auto ep_choice = book.choose(en_passant, 0, true, 16, 1);
    const auto promotion_choice = book.choose(promotion, 0, true, 16, 1);
    require(castle_choice && castle_choice->move == require_move("e1g1") && castle_choice->learn == 7,
            "Polyglot king-to-rook castling must decode to Koi king-destination castling");
    require(ep_choice && ep_choice->move == require_move("d5e6") && ep_choice->learn == 8,
            "Polyglot en-passant moves must decode to legal Koi moves");
    require(promotion_choice && promotion_choice->move == require_move("a7a8q") &&
                promotion_choice->learn == 9,
            "Polyglot promotions must decode to legal Koi promotion moves");
}

void test_book_filters_illegal_and_zero_weight_entries_and_is_seeded() {
    koi::test::TempDirectory files;
    const auto book_path = files.path() / "weighted.bin";
    const GameState state = GameState::startpos();
    write_book(book_path, {
        {state.polyglot_key(), polyglot_move("e2", "e5"), 1000, 1},
        {state.polyglot_key(), polyglot_move("g1", "f3"), 0, 2},
        {state.polyglot_key(), polyglot_move("e2", "e4"), 1, 3},
        {state.polyglot_key(), polyglot_move("d2", "d4"), 100, 4},
    });

    OpeningBook book(files.path());
    book.set_file(book_path);
    const auto first = book.choose(state, 0, true, 16, 42, true);
    const auto second = book.choose(state, 0, true, 16, 42, true);
    require(first && second && first->move == second->move,
            "a nonzero seed must select the same weighted legal book move repeatedly");
    require(first->move == require_move("e2e4") || first->move == require_move("d2d4"),
            "book selection must reject illegal and zero-weight records");
    require(state.is_legal(first->move), "a selected book move must be legal in the queried GameState");

    std::size_t e4_count = 0;
    std::size_t d4_count = 0;
    for (std::uint64_t seed = 1; seed <= 128; ++seed) {
        const auto choice = book.choose(state, 0, true, 16, seed, true);
        require(choice.has_value(), "positive-weight legal entries must be selectable");
        if (choice->move == require_move("e2e4")) {
            ++e4_count;
        } else if (choice->move == require_move("d2d4")) {
            ++d4_count;
        } else {
            require(false, "weighted selection must never return a filtered book entry");
        }
    }
    require(d4_count > e4_count, "higher weights must win more seeded selections than lower weights");
}

void test_book_defaults_to_highest_weight_and_coordinate_tie_breaking() {
    koi::test::TempDirectory files;
    const GameState state = GameState::startpos();
    const auto weighted = files.path() / "deterministic.bin";
    write_book(weighted, {
        {state.polyglot_key(), polyglot_move("e2", "e4"), 1, 0},
        {state.polyglot_key(), polyglot_move("d2", "d4"), 100, 0},
    });

    OpeningBook book(files.path());
    book.set_file(weighted);
    for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42}}) {
        const auto choice = book.choose(state, 0, true, 16, seed, false);
        require(choice && choice->move == require_move("d2d4"),
                "default book mode must choose the highest-weight legal move");
    }

    const auto tied = files.path() / "tied.bin";
    write_book(tied, {
        {state.polyglot_key(), polyglot_move("e2", "e4"), 100, 0},
        {state.polyglot_key(), polyglot_move("d2", "d4"), 100, 0},
    });
    book.set_file(tied);
    for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{42}}) {
        const auto choice = book.choose(state, 0, true, 16, seed, false);
        require(choice && choice->move == require_move("d2d4"),
                "equal book weights must use deterministic coordinate ordering");
    }
}

void test_book_falls_back_for_unavailable_or_unusable_inputs() {
    koi::test::TempDirectory files;
    const GameState state = GameState::startpos();
    OpeningBook book(files.path());
    book.set_file("missing.bin");
    require(!book.choose(state, 0, true, 16, 1), "a missing book must silently fall back");

    const auto malformed = files.path() / "malformed.bin";
    {
        std::ofstream stream(malformed, std::ios::binary);
        stream.put('x');
    }
    book.set_file(malformed);
    require(!book.choose(state, 0, true, 16, 1), "a non-record-aligned book must fall back");

    const auto legal = files.path() / "legal.bin";
    write_book(legal, {{state.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});
    book.set_file(legal);
    require(!book.choose(state, 0, false, 16, 1), "a disabled book must not be queried");
    require(!book.choose(state, 16, true, 16, 1), "maximum book depth must be exclusive");
    require(book.choose(state, 16, true, 0, 1).has_value(), "zero maximum depth must be unlimited");

    const auto only_illegal = files.path() / "only-illegal.bin";
    write_book(only_illegal, {{state.polyglot_key(), polyglot_move("e2", "e5"), 1, 0}});
    book.set_file(only_illegal);
    require(!book.choose(state, 0, true, 16, 1), "a book with no usable moves must fall back");
}

void test_book_rejects_oversized_record_aligned_input_without_throwing() {
    koi::test::TempDirectory files;
    const GameState state = GameState::startpos();
    const auto oversized = files.path() / "oversized.bin";
    write_book(oversized, {{state.polyglot_key(), polyglot_move("e2", "e4"), 1, 0}});

    constexpr std::uintmax_t oversized_size = 16U * 1024U * 1024U + 16U;
    std::error_code error;
    std::filesystem::resize_file(oversized, oversized_size, error);
    require(!error, "the oversized test book must be resizable");

    OpeningBook book(files.path());
    book.set_file(oversized);
    bool threw = false;
    std::optional<koi::BookChoice> choice;
    try {
        choice = book.choose(state, 0, true, 16, 1);
    } catch (...) {
        threw = true;
    }
    require(!threw, "an oversized book must fall back without throwing");
    require(!choice.has_value(), "an oversized book must be rejected before selection");
}

void test_book_safety_rejects_an_immediate_hanging_piece() {
    koi::test::TempDirectory files;
    const GameState state = require_state("k3r3/8/8/8/4Q3/8/8/K7 w - - 0 1");
    const auto book_path = files.path() / "unsafe.bin";
    write_book(book_path, {
        {state.polyglot_key(), polyglot_move("e4", "e3"), 100, 0},
        {state.polyglot_key(), polyglot_move("e4", "d4"), 1, 0},
    });

    OpeningBook book(files.path());
    book.set_file(book_path);
    const auto unsafe = book.choose(state, 0, true, 16, 1, false, true, 2);
    require(!unsafe.has_value(),
            "book safety must fall back when the highest-weight move immediately hangs a queen");

    const auto allowed = book.choose(state, 0, true, 16, 1, false, false, 2);
    require(allowed && allowed->move == require_move("e4e3"),
            "disabling book safety must preserve the highest-weight legal book move");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"reference Polyglot keys", test_polyglot_keys_match_reference_positions},
        {"special Polyglot moves", test_book_decodes_castling_en_passant_and_promotions},
        {"weighted legal selection", test_book_filters_illegal_and_zero_weight_entries_and_is_seeded},
        {"deterministic highest-weight selection", test_book_defaults_to_highest_weight_and_coordinate_tie_breaking},
        {"book fallback behavior", test_book_falls_back_for_unavailable_or_unusable_inputs},
        {"oversized book fallback", test_book_rejects_oversized_record_aligned_input_without_throwing},
        {"book safety", test_book_safety_rejects_an_immediate_hanging_piece},
    };

    return koi::test::run_tests(tests, argc, argv);
}
