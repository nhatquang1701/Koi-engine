#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/opening_book.hpp"

#ifdef CHESS_HPP
#error "Public Koi opening-book headers must not include chess.hpp"
#endif

namespace {

using koi::GameState;
using koi::Move;
using koi::OpeningBook;

struct BookRecord {
    std::uint64_t key;
    std::uint16_t move;
    std::uint16_t weight;
    std::uint32_t learn;
};

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

GameState require_state(std::string_view fen) {
    const auto state = GameState::from_fen(fen);
    require(state.has_value(), "test fixture FEN must be valid");
    return *state;
}

std::uint16_t polyglot_move(std::string_view from, std::string_view to,
                            std::uint8_t promotion = 0) {
    const auto source = koi::Square::parse(from);
    const auto target = koi::Square::parse(to);
    require(source.has_value() && target.has_value(), "Polyglot coordinate fixture must be valid");
    return static_cast<std::uint16_t>(source->index() | (target->index() << 6) | (promotion << 12));
}

template <class UInt>
void append_big_endian(std::vector<char>& bytes, UInt value) {
    for (int shift = static_cast<int>(sizeof(UInt) * 8) - 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

void write_book(const std::filesystem::path& path, const std::vector<BookRecord>& records) {
    std::vector<char> bytes;
    bytes.reserve(records.size() * 16);
    for (const BookRecord& record : records) {
        append_big_endian(bytes, record.key);
        append_big_endian(bytes, record.move);
        append_big_endian(bytes, record.weight);
        append_big_endian(bytes, record.learn);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good(), "test book must be writable");
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "test book must be completely written");
}

class TestDirectory {
public:
    TestDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("koi-opening-book-tests-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TestDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

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
    TestDirectory files;
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
    TestDirectory files;
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
    const auto first = book.choose(state, 0, true, 16, 42);
    const auto second = book.choose(state, 0, true, 16, 42);
    require(first && second && first->move == second->move,
            "a nonzero seed must select the same weighted legal book move repeatedly");
    require(first->move == require_move("e2e4") || first->move == require_move("d2d4"),
            "book selection must reject illegal and zero-weight records");
    require(state.is_legal(first->move), "a selected book move must be legal in the queried GameState");

    std::size_t e4_count = 0;
    std::size_t d4_count = 0;
    for (std::uint64_t seed = 1; seed <= 128; ++seed) {
        const auto choice = book.choose(state, 0, true, 16, seed);
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

void test_book_falls_back_for_unavailable_or_unusable_inputs() {
    TestDirectory files;
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

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"reference Polyglot keys", test_polyglot_keys_match_reference_positions},
        {"special Polyglot moves", test_book_decodes_castling_en_passant_and_promotions},
        {"weighted legal selection", test_book_filters_illegal_and_zero_weight_entries_and_is_seeded},
        {"book fallback behavior", test_book_falls_back_for_unavailable_or_unusable_inputs},
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
