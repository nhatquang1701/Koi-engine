#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi_test_support.hpp"

#include "koi/game_state.hpp"
#include "koi/position.hpp"
#include "koi/move_chooser.hpp"

namespace {

using koi::Move;
using koi::Position;
using koi::RandomMoveChooser;

static_assert(sizeof(Move) == sizeof(std::uint32_t),
              "Koi moves must remain 32-bit packed values");

constexpr std::string_view kInitialFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

using koi::test::require;

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
        first.castling_rights != second.castling_rights ||
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

std::uint64_t reference_feature_attacks(const koi::PositionFeatures& features,
                                        koi::Color color) {
    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    constexpr int king_directions[8][2] = {
        {1, 1}, {1, 0}, {1, -1}, {0, 1},
        {0, -1}, {-1, 1}, {-1, 0}, {-1, -1},
    };
    constexpr int bishop_directions[4][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    constexpr int rook_directions[4][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    };
    constexpr int queen_directions[8][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    };

    std::uint64_t attacks = 0;
    for (int source = 0; source < 64; ++source) {
        const koi::Piece piece = features.board[static_cast<std::size_t>(source)];
        if (piece.empty() || piece.color != color) {
            continue;
        }
        const int file = source % 8;
        const int rank = source / 8;
        const auto add_step = [&attacks, file, rank](int file_delta, int rank_delta) {
            const int target_file = file + file_delta;
            const int target_rank = rank + rank_delta;
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                attacks |= std::uint64_t{1} << (target_rank * 8 + target_file);
            }
        };
        if (piece.type == koi::PieceType::pawn) {
            const int direction = color == koi::Color::white ? 1 : -1;
            add_step(-1, direction);
            add_step(1, direction);
            continue;
        }
        if (piece.type == koi::PieceType::knight || piece.type == koi::PieceType::king) {
            const auto& directions = piece.type == koi::PieceType::knight ?
                knight_directions : king_directions;
            for (const auto& direction : directions) {
                add_step(direction[0], direction[1]);
            }
            continue;
        }

        const auto& directions = piece.type == koi::PieceType::bishop ? bishop_directions :
            (piece.type == koi::PieceType::rook ? rook_directions : queen_directions);
        const int direction_count = piece.type == koi::PieceType::queen ? 8 : 4;
        for (int direction = 0; direction < direction_count; ++direction) {
            int target_file = file + directions[direction][0];
            int target_rank = rank + directions[direction][1];
            while (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                const std::uint64_t target = std::uint64_t{1} << (target_rank * 8 + target_file);
                attacks |= target;
                if (!features.board[static_cast<std::size_t>(target_rank * 8 + target_file)].empty()) {
                    break;
                }
                target_file += directions[direction][0];
                target_rank += directions[direction][1];
            }
        }
    }
    return attacks;
}

std::uint64_t reference_king_zone(std::uint8_t square) {
    if (square >= 64) {
        return 0;
    }
    const int file = square % 8;
    const int rank = square / 8;
    std::uint64_t zone = 0;
    for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
        for (int file_delta = -1; file_delta <= 1; ++file_delta) {
            const int target_file = file + file_delta;
            const int target_rank = rank + rank_delta;
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                zone |= std::uint64_t{1} << (target_rank * 8 + target_file);
            }
        }
    }
    return zone;
}

void test_position_features_match_the_native_board_after_replay() {
    constexpr std::array<std::string_view, 4> fens{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/ppp2ppp/2n5/3Pp3/8/2N5/PPP2PPP/R3K2R w KQkq e6 0 2",
        "4k3/P6p/8/3b4/4B3/8/p6P/4K3 w - - 0 1",
        "r1bq1rk1/pp1n1ppp/2p1pn2/3p4/3P4/2N1PN2/PP1N1PPP/R1BQ1RK1 w - - 0 8",
    };
    constexpr std::uint64_t core_center_mask =
        (std::uint64_t{1} << 27) | (std::uint64_t{1} << 28) |
        (std::uint64_t{1} << 35) | (std::uint64_t{1} << 36);

    for (const std::string_view fen : fens) {
        auto state_result = koi::GameState::from_fen(fen);
        require(state_result.has_value(), "native feature replay FEN must be valid");
        koi::GameState state = *state_result;
        for (int ply = 0; ply < 10; ++ply) {
            const koi::PositionFeatures features = state.position_features();
            std::array<std::uint64_t, 2> expected_attacks{};
            std::array<std::uint64_t, 2> own_occupancy{};
            std::array<std::uint8_t, 2> expected_development{};
            std::array<std::uint8_t, 2> expected_pawn_files{};
            int expected_phase = 0;
            for (std::uint8_t square = 0; square < 64; ++square) {
                const koi::Piece expected = state.piece_at(koi::Square::from_index(square));
                require(features.board[square].type == expected.type &&
                            features.board[square].color == expected.color,
                        "native feature board must match the native piece map");
                if (expected.empty()) {
                    continue;
                }
                const std::size_t color = expected.color == koi::Color::white ? 0U : 1U;
                own_occupancy[color] |= std::uint64_t{1} << square;
                expected_attacks[color] = reference_feature_attacks(features, expected.color);
                if (expected.type == koi::PieceType::pawn) {
                    expected_pawn_files[color] = static_cast<std::uint8_t>(
                        expected_pawn_files[color] | (std::uint8_t{1} << (square % 8)));
                }
                if (expected.type == koi::PieceType::knight || expected.type == koi::PieceType::bishop) {
                    const bool home = expected.color == koi::Color::white ?
                        ((expected.type == koi::PieceType::knight && (square == 1 || square == 6)) ||
                         (expected.type == koi::PieceType::bishop && (square == 2 || square == 5))) :
                        ((expected.type == koi::PieceType::knight && (square == 57 || square == 62)) ||
                         (expected.type == koi::PieceType::bishop && (square == 58 || square == 61)));
                    if (!home) {
                        ++expected_development[color];
                    }
                }
                expected_phase += expected.type == koi::PieceType::knight ||
                    expected.type == koi::PieceType::bishop ? 1 :
                    expected.type == koi::PieceType::rook ? 2 :
                    expected.type == koi::PieceType::queen ? 4 : 0;
            }
            expected_phase = std::min(expected_phase, 24);
            for (const koi::Color color : {koi::Color::white, koi::Color::black}) {
                const std::size_t index = color == koi::Color::white ? 0U : 1U;
                require(features.attacked_squares[index] == expected_attacks[index],
                        "native feature attacks must match the board reference");
                require(features.mobility[index] ==
                            std::popcount(expected_attacks[index] & ~own_occupancy[index]),
                        "native feature mobility must match the board reference");
                require(features.pawn_file_masks[index] == expected_pawn_files[index] &&
                            features.development[index] == expected_development[index],
                        "native pawn and development features must match the board reference");
                require(features.center_control[index] ==
                            std::popcount(expected_attacks[index] & core_center_mask),
                        "native center-control features must match the board reference");
                const std::uint8_t king = features.king_squares[index].index();
                require(features.king_zone_attacks[index] == static_cast<std::uint8_t>(std::popcount(
                            expected_attacks[1U - index] & reference_king_zone(king))),
                        "native king-zone features must match the board reference");
            }
            require(features.game_phase == expected_phase,
                    "native feature game phase must match the material map");
            const auto legal = state.legal_moves();
            if (legal.empty() || !state.make_move(legal.front())) {
                break;
            }
        }
    }
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

    require(position.apply_uci("e2e4"), "e2e4 must be accepted");
    require(position.fen() == "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            "e2e4 must update the position");

    position.apply_uci("e7e5");
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
            "e7e5 must update the position");

    position.apply_uci("g1f3");
    require(position.fen() == "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
            "g1f3 must update the position");
}

void test_native_position_undo_restores_state_and_key() {
    Position position;
    const std::string initial_fen = position.fen();
    const std::uint64_t initial_key = position.position_key();

    require(position.apply_uci("e2e4"), "native undo fixture move must be legal");
    require(position.position_key() != initial_key,
            "a legal native move must change the incremental position key");
    require(position.unapply(), "native undo must restore the previous state");
    require(position.fen() == initial_fen && position.position_key() == initial_key,
            "native undo must restore FEN and incremental key exactly");
    require(!position.unapply(), "native undo must reject an empty undo stack");
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

void test_strict_material_and_double_check_limits_are_transactional() {
    Position position;
    const std::string original = position.fen();
    for (std::string_view fen : {
             "4k3/8/8/8/P7/PPPPPPPP/8/4K3 w - - 0 1",
             "4k3/8/8/8/8/N7/PPPPPPPP/RNBQKBNR w - - 0 1",
             "k7/8/7r/8/8/8/R7/7K w - - 0 1",
         }) {
        require(!position.set_fen(fen),
                "native production parsing must reject impossible material or double-check FEN");
        require(position.fen() == original,
                "rejected strict FEN must leave the native position unchanged");
    }
    require(position.set_fen("4k3/8/8/8/8/8/PPPPPPPP/RNBQKBNQ w - - 0 1"),
            "native parsing must retain legal promoted-material positions within limits");

    Position synthetic;
    require(synthetic.set_fen_unchecked("k7/8/7r/8/8/8/R7/7K w - - 0 1"),
            "fixture-only unchecked parsing must remain available for synthetic positions");
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

void test_native_position_exposes_search_state_contract() {
    Position position;
    require(position.side_to_move() == koi::Color::white,
            "native position must expose the side to move");
    require(position.piece_at(*koi::Square::parse("e2")).type == koi::PieceType::pawn,
            "native position must expose pieces by square");
    const auto e2e4 = koi::Move::parse_uci("e2e4");
    require(e2e4.has_value() && position.is_legal(*e2e4),
            "native position must validate legal moves");
    require(!position.is_capture(*e2e4),
            "native position must classify quiet moves");
    require(position.make_move(*e2e4),
            "native position must make a legal move");
    require(position.side_to_move() == koi::Color::black &&
                position.halfmove_clock() == 0 && position.fullmove_number() == 1,
            "native position counters must update after a move");
    require(position.unmake_move(),
            "native position must unmake a legal move");
    require(position.side_to_move() == koi::Color::white &&
                position.fen() == kInitialFen,
            "native position unmake must restore the complete state");
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

void test_fabricated_metadata_is_rejected_by_native_legality() {
    koi::GameState state = koi::GameState::startpos();
    const std::string original = state.fen();
    const auto move = koi::Move::parse_uci("e2e5");
    require(move.has_value(), "fabricated metadata move must parse");

    koi::MoveMetadata fabricated;
    fabricated.move = *move;
    fabricated.moving_piece = koi::PieceType::pawn;
    fabricated.kind = koi::MoveKind::quiet;
    fabricated.position_key = state.position_key();
    require(!state.make_legal_move(fabricated),
            "metadata with a matching key must still pass native legality");
    require(state.fen() == original,
            "rejected fabricated metadata must leave the state unchanged");
}

void test_tactical_generation_skips_quiet_check_probes_when_disabled() {
    koi::GameState state = koi::GameState::startpos();
    koi::MoveMetadataList moves;

    require(state.legal_tactical_moves_with_metadata(moves, false, true),
            "the start position must have legal moves even without quiet checks");
    require(moves.empty(),
            "tactical generation without quiet checks must return no quiets from the start position");

    const auto capture_result = koi::GameState::from_fen(
        "4k3/8/8/4r3/4Q3/8/8/4K3 w - - 0 1");
    require(capture_result.has_value(), "capture check fixture must be valid");
    koi::GameState capture_state = *capture_result;
    koi::MoveMetadataList capture_moves;
    require(capture_state.legal_tactical_moves_with_metadata(capture_moves, false, true),
            "capture check fixture must have legal moves");
    const auto checking_capture = std::find_if(
        capture_moves.begin(), capture_moves.end(), [](const koi::MoveMetadata& metadata) {
            return metadata.move.uci() == "e4e5";
        });
    require(checking_capture != capture_moves.end() && checking_capture->gives_check,
            "tactical generation must still probe captures for check annotations");
}

void test_fast_quiet_check_flags_match_exhaustive_move_descriptions() {
    const std::array<std::string_view, 6> fixtures{
        kInitialFen,
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
        "4k3/8/8/8/8/8/P7/4K3 w - - 0 1",
        "4k3/8/8/8/8/2N5/4N3/4R1K1 w - - 0 1",
        "4k3/7N/8/8/8/8/8/4K3 w - - 0 1",
    };

    const auto compare_position = [](const koi::GameState& state) {
        const std::vector<koi::MoveMetadata> exhaustive = state.legal_moves_with_metadata();
        koi::MoveMetadataList fast;
        state.legal_moves_with_metadata(fast, true, false, koi::CheckFlagMode::quiet_moves_only);

        for (const koi::MoveMetadata& expected : exhaustive) {
            if (expected.is_capture() || expected.move.promotion() != koi::Promotion::none) {
                continue;
            }
            const auto actual = std::find_if(fast.begin(), fast.end(),
                [&expected](const koi::MoveMetadata& metadata) {
                    return metadata.move == expected.move;
                });
            require(actual != fast.end(),
                    "fast quiet-check generation must retain every quiet legal move");
            if (actual->gives_check != expected.gives_check) {
                throw std::runtime_error(
                    "native quiet-check mismatch at " + state.fen() + " move " +
                    expected.move.uci() + " native=" +
                    (actual->gives_check ? "true" : "false") + " exhaustive=" +
                    (expected.gives_check ? "true" : "false"));
            }
        }
    };

    for (const std::string_view fen : fixtures) {
        const auto state = koi::GameState::from_fen(fen);
        require(state.has_value(), "quiet-check differential fixture must be valid");
        compare_position(*state);
    }

    std::mt19937 random(0x4B4F4951U);
    koi::GameState state = koi::GameState::startpos();
    for (int ply = 0; ply < 240; ++ply) {
        compare_position(state);
        const std::vector<koi::Move> legal = state.legal_moves();
        if (legal.empty()) {
            state = koi::GameState::startpos();
            continue;
        }
        const koi::Move move = legal[static_cast<std::size_t>(random() % legal.size())];
        require(state.make_move(move), "random differential replay move must be legal");
        if ((ply + 1) % 48 == 0) {
            state = koi::GameState::startpos();
        }
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
        {"native position undo and key", test_native_position_undo_restores_state_and_key},
        {"special moves", test_special_move_positions_accept_valid_uci_moves},
        {"malformed FEN rejection", test_malformed_fens_are_rejected_transactionally},
        {"back-rank pawn rejection", test_pawns_on_back_ranks_are_rejected_transactionally},
        {"en-passant state validation", test_incoherent_en_passant_targets_are_rejected_transactionally},
        {"adjacent king rejection", test_adjacent_kings_are_rejected_transactionally},
        {"castling-rights validation", test_castling_rights_require_the_standard_pieces},
        {"triple-check rejection", test_impossible_triple_check_is_rejected_transactionally},
        {"strict material and double-check rejection", test_strict_material_and_double_check_limits_are_transactional},
        {"invalid UCI rejection", test_invalid_uci_is_rejected_transactionally},
        {"malformed UCI rejection", test_malformed_uci_is_rejected_transactionally},
        {"compatibility position forwarding", test_position_forwards_fen_and_move_application},
        {"native search state contract", test_native_position_exposes_search_state_contract},
        {"checkmate and stalemate", test_checkmate_and_stalemate_have_no_legal_moves},
        {"random chooser legality", test_random_chooser_returns_a_legal_move},
        {"seeded chooser repeatability", test_seeded_choosers_are_repeatable},
        {"position feature freshness", test_position_features_refresh_after_make_and_unmake},
        {"position feature concurrent reads", test_position_features_are_safe_for_concurrent_const_reads},
        {"position feature concurrent copy", test_copying_a_const_state_is_safe_while_its_feature_cache_is_populated},
        {"native feature replay", test_position_features_match_the_native_board_after_replay},
        {"tactical checking horizon", test_tactical_generation_omits_quiet_checks_after_the_checking_horizon},
        {"metadata SEE cache", test_move_metadata_caches_see_and_rejects_stale_positions},
        {"fabricated metadata rejection", test_fabricated_metadata_is_rejected_by_native_legality},
        {"tactical quiet-check probe budget", test_tactical_generation_skips_quiet_check_probes_when_disabled},
        {"fast quiet-check differential", test_fast_quiet_check_flags_match_exhaustive_move_descriptions},
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
