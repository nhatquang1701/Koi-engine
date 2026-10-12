#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
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

// ---------------------------------------------------------------------------
// Reference exchange evaluator: the pre-bitboard mailbox algorithm, copied
// into the test so the differential compares the maintained-mask version
// against the exact implementation it replaced.  It shares only the stable
// material-value table with the engine and touches none of the exchange
// helpers under test.
// ---------------------------------------------------------------------------

namespace reference {

using koi::Color;
using koi::Move;
using koi::MoveKind;
using koi::Piece;
using koi::PieceType;
using koi::Promotion;

constexpr int piece_value(PieceType type) noexcept {
    return koi::piece_material_value(type);
}

constexpr PieceType promotion_piece(Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::knight:
        return PieceType::knight;
    case Promotion::bishop:
        return PieceType::bishop;
    case Promotion::rook:
        return PieceType::rook;
    case Promotion::queen:
        return PieceType::queen;
    case Promotion::none:
        return PieceType::pawn;
    }
    return PieceType::pawn;
}

constexpr int promotion_gain(Promotion promotion) noexcept {
    return piece_value(promotion_piece(promotion)) - piece_value(PieceType::pawn);
}

constexpr bool square_valid(int square) noexcept {
    return square >= 0 && square < 64;
}

// Old mailbox attack test: scan the board around the target.
bool square_attacked_by(const std::array<Piece, 64>& board, int target,
                        Color attacker) noexcept {
    if (!square_valid(target)) {
        return false;
    }

    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto has_piece = [&board, attacker](int square, PieceType type) noexcept {
        if (!square_valid(square)) {
            return false;
        }
        const Piece piece = board[static_cast<std::size_t>(square)];
        return piece.color == attacker && piece.type == type;
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if ((target_file > 0 && has_piece((pawn_rank << 3) + target_file - 1, PieceType::pawn)) ||
            (target_file < 7 && has_piece((pawn_rank << 3) + target_file + 1, PieceType::pawn))) {
            return true;
        }
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8 &&
            has_piece((rank << 3) + file, PieceType::knight)) {
            return true;
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            if (has_piece((rank << 3) + file, PieceType::king)) {
                return true;
            }
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const Piece piece = board[static_cast<std::size_t>((rank << 3) + file)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    return true;
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
    return false;
}

// Old mailbox attacker walk: pawns, knights, kings, then sliding rays that
// stop at the first occupied square.
template <typename CandidateHandler>
void for_each_attacker(const std::array<Piece, 64>& board, int target,
                       Color attacker, CandidateHandler&& handler) noexcept {
    if (!square_valid(target)) {
        return;
    }
    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto visit = [&board, attacker, &handler](int square, PieceType expected) noexcept {
        if (square_valid(square) &&
            board[static_cast<std::size_t>(square)].color == attacker &&
            board[static_cast<std::size_t>(square)].type == expected) {
            handler(square);
        }
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if (target_file > 0) visit((pawn_rank << 3) + target_file - 1, PieceType::pawn);
        if (target_file < 7) visit((pawn_rank << 3) + target_file + 1, PieceType::pawn);
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            visit((rank << 3) + file, PieceType::knight);
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            visit((rank << 3) + file, PieceType::king);
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const int square = (rank << 3) + file;
            const Piece piece = board[static_cast<std::size_t>(square)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    handler(square);
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
}

// Old trial-mutation legality probe: apply the recapture on the mailbox,
// rescan whether the king is attacked, then restore.
bool recapture_is_legal(std::array<Piece, 64>& board, std::array<int, 2>& king_squares,
                        Color side, int source, int target, Piece placed_piece) noexcept {
    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    const Piece captured_piece = board[static_cast<std::size_t>(target)];
    const std::size_t side_index = side == Color::white ? 0 : 1;
    const int saved_king_square = king_squares[side_index];

    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[side_index] = target;
    }
    const bool legal = !square_attacked_by(board, king_squares[side_index], koi::opposite(side));
    board[static_cast<std::size_t>(source)] = moving_piece;
    board[static_cast<std::size_t>(target)] = captured_piece;
    king_squares[side_index] = saved_king_square;
    return legal;
}

// Old whole-exchange evaluation over a mailbox snapshot of `state`.
int exchange_gain(const koi::GameState& state, const koi::MoveMetadata& initial) noexcept {
    std::array<Piece, 64> board{};
    state.copy_board_to(board);
    std::array<int, 2> king_squares{-1, -1};
    for (const Color color : {Color::white, Color::black}) {
        const std::uint64_t king = state.piece_bitboard(PieceType::king, color);
        if (king != 0) {
            king_squares[color == Color::white ? 0 : 1] = static_cast<int>(std::countr_zero(king));
        }
    }
    const Color side_to_move = state.side_to_move();

    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == koi::Square::kInvalid ||
        move.to().index() == koi::Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    const int source = move.from().index();
    const int target = move.to().index();
    if (!square_valid(source) || !square_valid(target)) {
        return 0;
    }

    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != side_to_move ||
        moving_piece.type != initial.moving_piece || king_squares[0] < 0 || king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_piece.color == Color::white ? -8 : 8;
    }
    if (!square_valid(captured_square)) {
        return 0;
    }

    constexpr std::size_t kMaximumExchangeDepth = 32;
    std::array<int, kMaximumExchangeDepth> gains{};
    const Piece captured_piece = board[static_cast<std::size_t>(captured_square)];
    gains[0] = piece_value(captured_piece.type) + promotion_gain(move.promotion());

    Piece placed_piece = moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_piece.type = promotion_piece(move.promotion());
    }
    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(captured_square)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[moving_piece.color == Color::white ? 0 : 1] = target;
    }

    Color side = koi::opposite(moving_piece.color);
    std::size_t depth = 0;
    for (;;) {
        int attacker_square = -1;
        int attacker_value = std::numeric_limits<int>::max();
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;

        for_each_attacker(board, target, side, [&](const int square) noexcept {
            const Piece candidate = board[static_cast<std::size_t>(square)];
            Piece replacement = candidate;
            int candidate_promotion_gain = 0;
            if (candidate.type == PieceType::pawn && (target >> 3 == 0 || target >> 3 == 7)) {
                replacement.type = PieceType::queen;
                candidate_promotion_gain =
                    piece_value(PieceType::queen) - piece_value(PieceType::pawn);
            }
            if (!recapture_is_legal(board, king_squares, side, square, target, replacement)) {
                return;
            }

            const int value = piece_value(candidate.type);
            if (value < attacker_value ||
                (value == attacker_value && (attacker_square < 0 || square < attacker_square))) {
                attacker_square = square;
                attacker_value = value;
                recapturing_piece = replacement;
                recapture_promotion_gain = candidate_promotion_gain;
            }
        });

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        const Piece target_piece = board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = piece_value(target_piece.type) - gains[depth - 1] +
            recapture_promotion_gain;

        const Piece departing_piece = board[static_cast<std::size_t>(attacker_square)];
        board[static_cast<std::size_t>(attacker_square)] = {};
        board[static_cast<std::size_t>(target)] = recapturing_piece;
        if (departing_piece.type == PieceType::king) {
            king_squares[side == Color::white ? 0 : 1] = target;
        }
        side = koi::opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }
    return gains[0];
}

} // namespace reference

// Differential bookkeeping for one comparison pass over a move list.
struct DifferentialResult {
    std::size_t comparisons = 0;
    std::size_t mismatches = 0;
    std::string first_mismatch;
};

// Compares the production entry point against the mailbox reference for every
// capture in `moves` (a legal generation of `state`).
void compare_captures(const koi::GameState& state, const koi::MoveMetadataList& moves,
                      DifferentialResult& result) {
    for (const koi::MoveMetadata& metadata : moves) {
        // Capturing promotions carry `kind == promotion` (not capture), so the
        // captured-piece field is the reliable capture signal for them.
        if (!metadata.is_capture() && metadata.captured_piece == koi::PieceType::none) {
            continue;
        }
        ++result.comparisons;
        const int expected = reference::exchange_gain(state, metadata);
        const int actual = koi::detail::static_exchange_gain(state, metadata);
        if (expected == actual) {
            continue;
        }
        ++result.mismatches;
        if (result.first_mismatch.empty()) {
            result.first_mismatch = state.fen() + " " + metadata.move.uci() + " expected " +
                std::to_string(expected) + " but got " + std::to_string(actual);
        }
    }
}

std::string_view mismatch_message(const DifferentialResult& result,
                                  std::string_view fallback) {
    return result.first_mismatch.empty() ? fallback : std::string_view(result.first_mismatch);
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

void test_randomized_differential_matches_the_mailbox_reference() {
    // Existing fixtures plus tactical middlegames; the differential compares
    // every legal capture of every position against the self-contained
    // pre-bitboard mailbox reference above.
    const std::vector<std::string_view> fixtures{
        "4k3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1",
        "3rk3/8/8/3p4/4Q3/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/4Q3/8/8/4K3 w - - 0 1",
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/8/1R6/2p5/1n2K3 w - - 0 1",
        "4k3/4n3/8/3p4/2Q5/8/8/4R1K1 w - - 0 1",
        "8/8/3k4/3p4/4Q3/8/8/3R2K1 w - - 0 1",
        "4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 2",
        "4k3/8/8/8/3pP3/8/8/4K3 b - e3 0 1",
        "4k3/8/8/8/8/8/1p6/R3K3 b - - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    };

    DifferentialResult fixture_result;
    for (const std::string_view fen : fixtures) {
        const koi::GameState state = require_state(fen);
        koi::MoveMetadataList moves;
        state.legal_moves_with_metadata(moves, false, false);
        compare_captures(state, moves, fixture_result);
    }
    require(fixture_result.mismatches == 0,
            mismatch_message(fixture_result,
                             "fixture captures must match the mailbox reference"));

    // Explicit pinned-recapturer, en-passant, and promotion coverage: each of
    // these must enumerate at least one capture so those paths are definitely
    // part of the differential.
    for (const std::string_view fen :
         {"4k3/4n3/8/3p4/2Q5/8/8/4R1K1 w - - 0 1",
          "4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 2",
          "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1"}) {
        const koi::GameState state = require_state(fen);
        koi::MoveMetadataList moves;
        state.legal_moves_with_metadata(moves, false, false);
        DifferentialResult explicit_result;
        compare_captures(state, moves, explicit_result);
        require(explicit_result.comparisons > 0,
                "the pinned/en-passant/promotion fixture must enumerate a capture");
        require(explicit_result.mismatches == 0,
                mismatch_message(explicit_result,
                                 "explicit fixture captures must match the mailbox reference"));
    }

    // Deterministic randomized playouts: every capture of every visited node
    // is compared against the mailbox reference.  The mt19937 seed is fixed so
    // the comparison count and the covered captures are reproducible.
    std::mt19937 random(0x53454551U);
    constexpr int kPlayoutsPerStart = 8;
    constexpr int kPlayoutPlies = 48;
    DifferentialResult fuzz_result;
    for (const std::string_view fen : fixtures) {
        for (int game = 0; game < kPlayoutsPerStart; ++game) {
            koi::GameState state = require_state(fen);
            for (int ply = 0; ply < kPlayoutPlies; ++ply) {
                koi::MoveMetadataList moves;
                state.legal_moves_with_metadata(moves, false, false);
                if (moves.empty()) {
                    break;
                }
                compare_captures(state, moves, fuzz_result);
                const std::size_t pick = static_cast<std::size_t>(random()) % moves.size();
                require(state.make_generated_move(moves[pick]),
                        "a randomly chosen legal move must apply");
            }
        }
    }
    require(fuzz_result.mismatches == 0,
            mismatch_message(fuzz_result,
                             "randomized captures must match the mailbox reference"));

    // The differential must have actually compared captures, with the
    // playouts contributing a meaningful sample on top of the fixtures.
    require(fixture_result.comparisons > 0,
            "the fixture pass must compare at least one capture");
    require(fuzz_result.comparisons > 100,
            "the randomized playouts must compare a meaningful number of captures");
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
        {"randomized differential against the mailbox reference",
         test_randomized_differential_matches_the_mailbox_reference},
    };
    return koi::test::run_tests(tests, argc, argv);
}
