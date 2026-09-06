#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/game_state.hpp"
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

bool same_features(const koi::PositionFeatures& first, const koi::PositionFeatures& second) {
    if (first.attacked_squares != second.attacked_squares || first.mobility != second.mobility ||
        first.king_squares != second.king_squares || first.pawn_file_masks != second.pawn_file_masks ||
        first.development != second.development || first.center_control != second.center_control ||
        first.king_zone_attacks != second.king_zone_attacks ||
        first.game_phase != second.game_phase || first.side_to_move != second.side_to_move) {
        return false;
    }
    for (std::size_t square = 0; square < first.board.size(); ++square) {
        if (first.board[square].type != second.board[square].type ||
            first.board[square].color != second.board[square].color) {
            return false;
        }
    }
    return true;
}

bool contains_metadata_move(const koi::MoveMetadataList& moves, std::string_view expected) {
    for (const koi::MoveMetadata& metadata : moves) {
        if (metadata.move.uci() == expected) {
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
             "4k3/8/8/8/8/8/8/4K3 w - - 256 1",
             "4k3/8/8/8/8/8/8/4K3 w - - 0 32769",
         }) {
        require(!position.set_fen(fen), "malformed or kingless FEN must be rejected");
        require(position.fen() == original, "rejected FEN must leave the position unchanged");
    }
}

void test_pawns_on_back_ranks_are_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    for (std::string_view fen : {
             "4k3/8/8/8/8/8/8/P3K3 w - - 0 1",
             "p3k3/8/8/8/8/8/8/4K3 w - - 0 1",
         }) {
        require(!position.set_fen(fen), "FEN with a pawn on rank one or eight must be rejected");
        require(position.fen() == original, "rejected back-rank pawn FEN must leave the position unchanged");
    }
}

void test_incoherent_en_passant_targets_are_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    for (std::string_view fen : {
             "4k3/8/8/8/8/8/8/4K3 w - e6 0 1",
             "4k3/8/8/4p3/8/8/8/4K3 w - e6 1 1",
             "4k3/4P3/8/4p3/8/8/8/4K3 w - e6 0 1",
         }) {
        require(!position.set_fen(fen), "en-passant target must describe the immediately preceding pawn double push");
        require(position.fen() == original, "rejected en-passant FEN must leave the position unchanged");
    }

    require(position.set_fen("4k3/8/8/4p3/8/8/8/4K3 w - e6 0 1"),
            "a coherent en-passant target must remain valid even without a capture available");
}

void test_adjacent_kings_are_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.set_fen("8/8/8/8/8/8/4k3/4K3 w - - 0 1"),
            "FEN with adjacent kings must be rejected");
    require(position.fen() == original, "rejected adjacent-kings FEN must leave the position unchanged");
}

void test_castling_rights_require_the_standard_pieces() {
    Position position;
    const std::string original = position.fen();

    for (std::string_view fen : {
             "4k3/8/8/8/8/8/8/3K2R1 w K - 0 1",
             "4k3/8/8/8/8/8/8/R2K4 w Q - 0 1",
             "r3k3/8/8/8/8/8/8/4K3 b k - 0 1",
             "3rk3/8/8/8/8/8/8/4K3 b q - 0 1",
         }) {
        require(!position.set_fen(fen), "castling rights must require the matching king and rook");
        require(position.fen() == original, "rejected castling FEN must leave the position unchanged");
    }
}

void test_impossible_triple_check_is_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.set_fen("k3r3/8/8/8/1b6/8/2n5/4K3 w - - 0 1"),
            "a triple-check FEN must be rejected before move generation");
    require(position.fen() == original, "rejected triple-check FEN must leave the position unchanged");
}

void test_invalid_uci_is_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.apply_uci("e2e5"), "illegal UCI move must be rejected");
    require(position.fen() == original, "rejected UCI move must leave the position unchanged");
}

void test_malformed_uci_is_rejected_transactionally() {
    Position position;
    const std::string original = position.fen();

    require(!position.apply_uci("not-a-move"), "malformed UCI input must be rejected");
    require(position.fen() == original, "malformed UCI input must leave the position unchanged");
}

void test_position_forwards_fen_and_move_application() {
    Position position;
    require(position.apply_uci("e2e4"), "e2e4 must be legal");
    require(position.apply_uci("e7e5"), "e7e5 must be legal");

    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            "compatibility position must forward FEN and move application");
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
    const koi::GameState state;
    RandomMoveChooser chooser(1234);

    const Move selected = chooser.choose(state);
    require(state.is_legal(selected), "random chooser must return a legal move");
}

void test_seeded_choosers_are_repeatable() {
    const koi::GameState state;
    RandomMoveChooser first(5678);
    RandomMoveChooser second(5678);

    for (int i = 0; i < 12; ++i) {
        require(first.choose(state).uci() == second.choose(state).uci(),
                "choosers with the same seed must produce the same sequence");
    }
}

void test_position_features_refresh_after_make_and_unmake() {
    koi::GameState state = koi::GameState::startpos();
    const koi::Move move = *koi::Move::parse_uci("e2e4");
    const auto before = state.position_features();
    require(state.make_move(move), "feature-cache fixture move must be legal");
    const auto advanced = state.position_features();
    require(advanced.board[28].type == koi::PieceType::pawn && advanced.board[12].empty(),
            "feature extraction must reflect the position after a pawn advance");
    require(state.unmake_move(), "feature-cache fixture move must unmake");
    const auto restored = state.position_features();
    require(same_features(restored, before),
            "feature extraction must not return stale data after make and unmake");
}

void test_position_features_are_safe_for_concurrent_const_reads() {
    const koi::GameState expected_state = koi::GameState::startpos();
    const koi::PositionFeatures expected = expected_state.position_features();
    const koi::GameState state = koi::GameState::startpos();
    std::atomic_bool start = false;
    std::atomic_bool mismatch = false;
    std::atomic_uint ready = 0;
    std::vector<std::thread> readers;
    readers.reserve(8);
    for (int reader = 0; reader < 8; ++reader) {
        readers.emplace_back([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int iteration = 0; iteration < 256; ++iteration) {
                if (!same_features(state.position_features(), expected)) {
                    mismatch.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != readers.size()) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }
    require(!mismatch.load(std::memory_order_relaxed),
            "concurrent const feature reads must return complete identical position features");
}

void test_copying_a_const_state_is_safe_while_its_feature_cache_is_populated() {
    const koi::GameState expected_state = koi::GameState::startpos();
    const koi::PositionFeatures expected = expected_state.position_features();
    std::atomic_bool mismatch = false;
    constexpr int kRounds = 128;
    constexpr int kCopiers = 4;
    constexpr int kCopiesBeforePublication = 32;
    for (int round = 0; round < kRounds; ++round) {
        const koi::GameState state = koi::GameState::startpos();
        std::atomic_bool start = false;
        std::atomic_bool publication_started = false;
        std::atomic_bool published = false;
        std::atomic_uint ready = 0;
        std::atomic_uint copy_workers_ready_for_publication = 0;
        std::atomic_uint copy_workers_in_publication_window = 0;
        std::atomic_uint copy_pairs_after_synchronization = 0;
        std::thread cache_populator([&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (copy_workers_ready_for_publication.load(std::memory_order_acquire) != kCopiers) {
                std::this_thread::yield();
            }
            publication_started.store(true, std::memory_order_release);
            while (copy_workers_in_publication_window.load(std::memory_order_acquire) != kCopiers) {
                std::this_thread::yield();
            }
            if (!same_features(state.position_features(), expected)) {
                mismatch.store(true, std::memory_order_relaxed);
            }
            published.store(true, std::memory_order_release);
        });
        std::vector<std::thread> copiers;
        copiers.reserve(kCopiers);
        for (int copier = 0; copier < kCopiers; ++copier) {
            copiers.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const auto copy_and_check = [&] {
                    const koi::GameState constructed(state);
                    koi::GameState assigned;
                    assigned = state;
                    if (!same_features(constructed.position_features(), expected) ||
                        !same_features(assigned.position_features(), expected)) {
                        mismatch.store(true, std::memory_order_relaxed);
                    }
                };
                for (int iteration = 0; iteration < kCopiesBeforePublication; ++iteration) {
                    copy_and_check();
                }
                copy_workers_ready_for_publication.fetch_add(1, std::memory_order_release);
                while (!publication_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                copy_workers_in_publication_window.fetch_add(1, std::memory_order_release);
                while (!published.load(std::memory_order_acquire)) {
                    copy_and_check();
                    copy_pairs_after_synchronization.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kCopiers + 1) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        cache_populator.join();
        for (std::thread& copier : copiers) {
            copier.join();
        }
        require(copy_pairs_after_synchronization.load(std::memory_order_relaxed) > 0,
                "each fresh cache publication must overlap synchronized copy pressure");
    }
    require(!mismatch.load(std::memory_order_relaxed),
            "copying fresh const states during cache publication must preserve complete features");
}

void test_tactical_generation_omits_quiet_checks_after_the_checking_horizon() {
    const auto quiet_check_state = koi::GameState::from_fen("k7/8/8/8/8/8/4Q3/4K3 w - - 0 1");
    require(quiet_check_state.has_value(), "quiet-check tactical fixture must be valid");
    koi::MoveMetadataList tactical;
    require(quiet_check_state->legal_tactical_moves_with_metadata(tactical, false),
            "quiet-check tactical fixture must have legal moves");
    require(!contains_metadata_move(tactical, "e2e8"),
            "the checking horizon must omit quiet checks from tactical generation");

    const auto capture_state = koi::GameState::from_fen("k7/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    require(capture_state.has_value(), "capture tactical fixture must be valid");
    require(capture_state->legal_tactical_moves_with_metadata(tactical, false),
            "capture tactical fixture must have legal moves");
    require(contains_metadata_move(tactical, "e4d5"),
            "the checking horizon must retain legal captures");

    const auto promotion_state = koi::GameState::from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    require(promotion_state.has_value(), "promotion tactical fixture must be valid");
    require(promotion_state->legal_tactical_moves_with_metadata(tactical, false),
            "promotion tactical fixture must have legal moves");
    for (const std::string_view promotion : {"q", "r", "b", "n"}) {
        require(contains_metadata_move(tactical, "a7a8" + std::string(promotion)),
                "the checking horizon must retain every legal promotion");
    }

    const auto evasion_state = koi::GameState::from_fen("k3r3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(evasion_state.has_value(), "evasion tactical fixture must be valid");
    const std::vector<koi::Move> evasions = evasion_state->legal_moves();
    require(evasion_state->legal_tactical_moves_with_metadata(tactical, false),
            "evasion tactical fixture must have legal moves");
    require(tactical.size() == evasions.size(), "checked tactical generation must retain every legal evasion");
    for (const koi::Move& evasion : evasions) {
        require(contains_metadata_move(tactical, evasion.uci()),
                "checked tactical generation must retain each legal evasion");
    }
}

void test_move_metadata_caches_see_and_rejects_stale_positions() {
    const auto state_result = koi::GameState::from_fen(
        "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    require(state_result.has_value(), "metadata cache fixture must be valid");
    koi::GameState state = *state_result;
    const std::uint64_t original_key = state.position_key();
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves);

    const auto selected = std::find_if(moves.begin(), moves.end(), [](const koi::MoveMetadata& metadata) {
        return metadata.move.uci() == "e4d5";
    });
    require(selected != moves.end(), "metadata cache fixture must include the queen capture");
    require(selected->position_key == original_key,
            "generated metadata must carry the position identity it describes");
    require(selected->see_score >= 500,
            "generated captures must cache a materially useful SEE score");

    const koi::MoveMetadata capture = *selected;
    require(state.make_legal_move(capture), "fresh generated metadata must be fast-applicable");
    require(!state.make_legal_move(capture),
            "metadata from a previous position must be rejected as stale");
    require(state.unmake_move(), "stale metadata fixture must unmake cleanly");
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
        {"back-rank pawn rejection", test_pawns_on_back_ranks_are_rejected_transactionally},
        {"en-passant state validation", test_incoherent_en_passant_targets_are_rejected_transactionally},
        {"adjacent king rejection", test_adjacent_kings_are_rejected_transactionally},
        {"castling-rights validation", test_castling_rights_require_the_standard_pieces},
        {"triple-check rejection", test_impossible_triple_check_is_rejected_transactionally},
        {"invalid UCI rejection", test_invalid_uci_is_rejected_transactionally},
        {"malformed UCI rejection", test_malformed_uci_is_rejected_transactionally},
        {"compatibility position forwarding", test_position_forwards_fen_and_move_application},
        {"checkmate and stalemate", test_checkmate_and_stalemate_have_no_legal_moves},
        {"random chooser legality", test_random_chooser_returns_a_legal_move},
        {"seeded chooser repeatability", test_seeded_choosers_are_repeatable},
        {"position feature freshness", test_position_features_refresh_after_make_and_unmake},
        {"position feature concurrent reads", test_position_features_are_safe_for_concurrent_const_reads},
        {"position feature concurrent copy", test_copying_a_const_state_is_safe_while_its_feature_cache_is_populated},
        {"tactical checking horizon", test_tactical_generation_omits_quiet_checks_after_the_checking_horizon},
        {"metadata SEE cache", test_move_metadata_caches_see_and_rejects_stale_positions},
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
