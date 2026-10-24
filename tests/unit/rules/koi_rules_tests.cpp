#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"

#include "koi_test_support.hpp"

#ifdef CHESS_HPP
#error "Public Koi rules headers must not include chess.hpp"
#endif

namespace {

using koi::GameState;
using koi::Move;
using koi::Promotion;
using koi::Square;

constexpr std::string_view kInitialFen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

using koi::test::require;

Move require_move(std::string_view uci) {
    return koi::test::require_value(Move::parse_uci(uci), "test move must parse");
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

void test_strict_fen_rejects_impossible_material_and_check_state() {
    const auto too_many_pawns = GameState::from_fen(
        "4k3/8/8/8/P7/PPPPPPPP/8/4K3 w - - 0 1");
    require(!too_many_pawns.has_value(),
            "production FEN parsing must reject more than eight pawns for one side");

    const auto too_many_pieces = GameState::from_fen(
        "4k3/8/8/8/8/N7/PPPPPPPP/RNBQKBNR w - - 0 1");
    require(!too_many_pieces.has_value(),
            "production FEN parsing must reject more than sixteen pieces for one side");

    const auto both_kings_in_check = GameState::from_fen(
        "k7/8/7r/8/8/8/R7/7K w - - 0 1");
    require(!both_kings_in_check.has_value(),
            "production FEN parsing must reject simultaneous check on both kings");

    const auto promoted_material = GameState::from_fen(
        "4k3/8/8/8/8/8/PPPPPPPP/RNBQKBNQ w - - 0 1");
    require(promoted_material.has_value(),
            "legal promoted-material positions within per-side limits must remain valid");
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

void test_special_move_conversion_round_trips_without_uci_translation() {
    struct Fixture {
        std::string_view fen;
        std::string_view move;
        std::string_view after;
    };
    constexpr Fixture fixtures[] = {
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1",
         "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1"},
        {"rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2", "d5e6",
         "rnbqkbnr/pppp1ppp/4P3/8/8/8/PPP1PPPP/RNBQKBNR b KQkq - 0 2"},
        {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q",
         "Q3k3/8/8/8/8/8/8/4K3 b - - 0 1"},
    };

    for (const Fixture& fixture : fixtures) {
        const auto parsed = GameState::from_fen(fixture.fen);
        require(parsed.has_value(), "special-move fixture FEN must be valid");
        GameState state = *parsed;
        require(state.make_move(require_move(fixture.move)),
                "special move must convert and apply as a legal native move");
        require(state.fen() == fixture.after, "special move conversion must preserve native board semantics");
        require(state.unmake_move(), "special move must have an undo record");
        require(state.fen() == fixture.fen, "special move conversion must round-trip the original position");
    }
}

void test_move_metadata_identifies_special_moves_and_checks() {
    const auto castling = GameState::from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    const auto en_passant = GameState::from_fen("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    const auto promotion = GameState::from_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    const auto checking = GameState::from_fen("4k3/8/8/8/8/8/4Q3/4K3 w - - 0 1");

    require(castling && en_passant && promotion && checking, "metadata fixture FENs must be valid");
    const auto castle = castling->describe_move(require_move("e1g1"));
    const auto ep = en_passant->describe_move(require_move("d5e6"));
    const auto promote = promotion->describe_move(require_move("a7a8q"));
    const auto check = checking->describe_move(require_move("e2e7"));

    require(castle.has_value() && castle->kind == koi::MoveKind::castling,
            "metadata must identify castling");
    require(ep.has_value() && ep->kind == koi::MoveKind::en_passant &&
                ep->captured_piece == koi::PieceType::pawn,
            "metadata must identify en-passant captures");
    require(promote.has_value() && promote->kind == koi::MoveKind::promotion,
            "metadata must identify promotions");
    require(check.has_value() && check->gives_check,
            "metadata must identify direct checking moves");
}

void test_batch_move_metadata_matches_individual_descriptions() {
    const auto state = GameState::from_fen(
        "r3k2r/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/R3K2R w KQkq e6 0 2");
    require(state.has_value(), "batch metadata fixture must be valid");

    const std::vector<koi::MoveMetadata> batch = state->legal_moves_with_metadata();
    const std::vector<Move> legal = state->legal_moves();
    require(batch.size() == legal.size(),
            "batch metadata must contain exactly one entry per legal move");

    koi::MoveMetadataList fixed_batch;
    state->legal_moves_with_metadata(fixed_batch);
    require(fixed_batch.size() == batch.size(),
            "fixed-capacity metadata storage must contain the complete legal list");

    for (const koi::MoveMetadata& metadata : batch) {
        const auto individual = state->describe_move(metadata.move);
        require(individual.has_value(), "batch metadata move must remain legal");
        require(individual->moving_piece == metadata.moving_piece &&
                    individual->captured_piece == metadata.captured_piece &&
                    individual->kind == metadata.kind &&
                    individual->gives_check == metadata.gives_check,
                    "batch metadata must match the individual move description");
    }
    for (std::size_t index = 0; index < batch.size(); ++index) {
        require(fixed_batch[index].move == batch[index].move &&
                    fixed_batch[index].kind == batch[index].kind &&
                    fixed_batch[index].gives_check == batch[index].gives_check,
                "fixed-capacity metadata storage must preserve generated move order");
    }
}

void test_search_metadata_path_can_skip_check_analysis() {
    const auto state = GameState::from_fen("k7/8/8/8/8/8/4Q3/4K3 w - - 0 1");
    require(state.has_value(), "search metadata fixture must be valid");

    koi::MoveMetadataList metadata;
    state->legal_moves_with_metadata(metadata, false);
    const auto checking_move = require_move("e2e8");
    const auto found = std::find_if(metadata.begin(), metadata.end(),
                                    [&checking_move](const koi::MoveMetadata& candidate) {
                                        return candidate.move == checking_move;
                                    });
    require(found != metadata.end() && !found->gives_check,
            "the search metadata path must preserve moves without computing check flags");
}

void test_generated_move_application_preserves_special_move_semantics() {
    struct Fixture {
        std::string_view fen;
        std::string_view move;
        std::string_view after;
    };
    constexpr Fixture fixtures[] = {
        {"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1",
         "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1"},
        {"rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2", "d5e6",
         "rnbqkbnr/pppp1ppp/4P3/8/8/8/PPP1PPPP/RNBQKBNR b KQkq - 0 2"},
        {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q",
         "Q3k3/8/8/8/8/8/8/4K3 b - - 0 1"},
    };

    for (const Fixture& fixture : fixtures) {
        const auto parsed = GameState::from_fen(fixture.fen);
        require(parsed.has_value(), "generated-move fixture must be valid");
        GameState state = *parsed;
        const auto wanted = require_move(fixture.move);
        const auto metadata = state.legal_moves_with_metadata();
        const auto selected = std::find_if(metadata.begin(), metadata.end(),
                                           [&wanted](const koi::MoveMetadata& candidate) {
                                               return candidate.move == wanted;
                                           });
        require(selected != metadata.end(), "fixture move must be in the generated legal list");
        require(state.make_legal_move(*selected), "a generated legal move must be applicable");
        require(state.fen() == fixture.after,
                "generated move application must preserve native special-move semantics");
        require(state.unmake_move(), "generated move must have an undo record");
        require(state.fen() == fixture.fen, "unmaking a generated move must restore the original position");
    }
}

void test_every_generated_move_can_use_the_fast_application_path() {
    const auto parsed = GameState::from_fen("4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    require(parsed.has_value(), "fast-application fixture must be valid");
    GameState state = *parsed;
    const std::string original_fen = state.fen();
    const std::uint64_t original_key = state.position_key();

    const auto metadata = state.legal_moves_with_metadata();
    require(!metadata.empty(), "fast-application fixture must have legal moves");
    for (const koi::MoveMetadata& candidate : metadata) {
        require(state.is_legal(candidate.move),
                "the metadata batch must contain only moves accepted as legal");
        require(state.make_legal_move(candidate), "every generated move must be fast-applicable");
        require(state.unmake_move(), "every fast-applied move must be undoable");
        require(state.fen() == original_fen && state.position_key() == original_key,
                "fast application must restore the complete source position");
    }
}

void test_draw_rule_view_separates_draws_from_move_exhaustion() {
    const GameState start = GameState::startpos();
    const auto dead_material = GameState::from_fen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    const auto fifty_move = GameState::from_fen("4k3/8/8/8/8/8/8/R3K3 w - - 100 1");

    require(!start.is_draw_by_rule(), "the starting position must not be a draw by rule");
    require(dead_material.has_value() && dead_material->is_draw_by_rule(),
            "insufficient material must be reported as a draw by rule");
    require(fifty_move.has_value() && !fifty_move->legal_moves().empty() &&
                fifty_move->is_draw_by_rule(),
            "a legal position at the fifty-move threshold must be a draw by rule");
}

void test_non_pawn_material_view_is_available_for_search_safety_checks() {
    const GameState start = GameState::startpos();
    const auto pawn_only = GameState::from_fen("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1");
    require(pawn_only.has_value(), "non-pawn material fixture must be valid");
    require(start.has_non_pawn_material(koi::Color::white) &&
                start.has_non_pawn_material(koi::Color::black),
            "the starting position must have non-pawn material for both sides");
    require(!pawn_only->has_non_pawn_material(koi::Color::white) &&
                !pawn_only->has_non_pawn_material(koi::Color::black),
            "a king-and-pawn position must not report non-pawn material");
}

void test_tactical_move_generation_excludes_quiet_moves_but_keeps_checks() {
    const auto capture_state = GameState::from_fen("k7/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    require(capture_state.has_value(), "tactical capture fixture must be valid");

    koi::MoveMetadataList tactical;
    const bool has_legal_move = capture_state->legal_tactical_moves_with_metadata(tactical);
    require(has_legal_move && !tactical.empty(),
            "tactical generation must report legal forcing moves");
    require(tactical.size() < capture_state->legal_moves().size(),
            "tactical generation must exclude quiet legal moves");
    for (const koi::MoveMetadata& metadata : tactical) {
        require(metadata.is_capture() || metadata.move.promotion() != koi::Promotion::none ||
                    metadata.gives_check,
                "tactical generation must contain only captures, promotions, or checks");
    }

    const auto check_state = GameState::from_fen("k7/8/8/8/8/8/4Q3/4K3 w - - 0 1");
    require(check_state.has_value(), "tactical check fixture must be valid");
    tactical.clear();
    (void)check_state->legal_tactical_moves_with_metadata(tactical);
    const auto checking_move = require_move("e2e8");
    const auto checking = std::find_if(tactical.begin(), tactical.end(),
                                       [&checking_move](const koi::MoveMetadata& metadata) {
                                           return metadata.move == checking_move;
                                       });
    require(checking != tactical.end() && checking->gives_check,
            "tactical generation must retain quiet checking moves");
}

void test_position_features_expose_symmetric_board_attacks_and_mobility() {
    const GameState state = GameState::startpos();
    const koi::PositionFeatures features = state.position_features();

    require(features.side_to_move == koi::Color::white, "features must retain the side to move");
    require(features.board[Square::parse("e1")->index()].type == koi::PieceType::king,
            "features must expose the board without native library types");
    require(features.king_squares[0] == *Square::parse("e1") &&
                features.king_squares[1] == *Square::parse("e8"),
            "features must expose both king squares");
    require(features.attacked_squares[0] != 0 && features.attacked_squares[1] != 0,
            "features must expose attacks for both colors");
    require(features.mobility[0] > 0 && features.mobility[1] > 0,
            "features must expose nonzero mobility for both colors");
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

void test_null_move_round_trips_side_fen_and_key() {
    GameState state = GameState::startpos();
    const std::string original_fen = state.fen();
    const std::uint64_t original_key = state.position_key();

    require(state.make_null_move(), "a legal search null move must be applicable");
    require(state.side_to_move() == koi::Color::black, "a null move must switch the side to move");
    require(state.unmake_null_move(), "a null move must be undoable");
    require(state.fen() == original_fen && state.position_key() == original_key &&
                state.side_to_move() == koi::Color::white,
            "unmaking a null move must restore the complete position");
    require(!state.unmake_null_move(), "an empty null-move history must be rejected");
}

void test_null_move_incremental_key_matches_a_fresh_parse() {
    struct Fixture {
        std::string_view fen;
        std::string_view expected_after;
        koi::Color side_after;
    };
    constexpr Fixture fixtures[] = {
        {"r3k2r/ppp2ppp/2n1bn2/3qp3/3P4/2NB1N2/PPP2PPP/R2Q1RK1 w kq - 10 9",
         "r3k2r/ppp2ppp/2n1bn2/3qp3/3P4/2NB1N2/PPP2PPP/R2Q1RK1 b kq - 10 9", koi::Color::black},
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
         "4k3/8/8/3pP3/8/8/8/4K3 b - - 0 2", koi::Color::black},
        {"4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
         "4r1k1/8/8/3pP3/8/8/8/4K3 b - - 0 2", koi::Color::black},
        {"4k3/8/3p4/8/4Pp2/8/8/4K3 b - e3 0 9",
         "4k3/8/3p4/8/4Pp2/8/8/4K3 w - - 0 10", koi::Color::white},
    };

    for (const Fixture& fixture : fixtures) {
        const auto parsed = GameState::from_fen(fixture.fen);
        require(parsed.has_value(), "null-move key fixture FEN must parse");
        GameState state = *parsed;
        require(state.make_null_move(), "a fixture must accept a null move");

        const auto after = GameState::from_fen(fixture.expected_after);
        require(after.has_value(), "null-move expected FEN must parse");
        require(state.fen() == fixture.expected_after,
                "a null move must emit the expected FEN");
        require(state.side_to_move() == fixture.side_after,
                "a null move must flip the side to move");
        require(state.position_key() == after->position_key(),
                "the null-move key must match a fresh parse of the resulting FEN");
    }
}

void test_rejected_king_move_leaves_state_unchanged() {
    const auto parsed = GameState::from_fen("4r2k/8/8/8/8/8/8/4K3 w - - 0 1");
    require(parsed.has_value(), "rejected-move fixture must construct");
    GameState state = *parsed;
    const std::string original_fen = state.fen();
    const std::uint64_t original_key = state.position_key();

    require(!state.make_move(require_move("e1e2")),
            "a king move that remains on the checking rook file must be rejected");
    require(state.fen() == original_fen, "a rejected move must preserve FEN");
    require(state.position_key() == original_key,
            "a rejected move must preserve the position key");
    require(!state.unmake_move(), "a rejected move must not add an undo entry");
}

void test_start_position_has_twenty_legal_moves() {
    const GameState state = GameState::startpos();
    require(state.fen() == kInitialFen, "startpos must use the standard initial FEN");
    require(state.legal_moves().size() == 20, "startpos must expose exactly twenty legal moves");
}

void test_legal_move_existence_matches_the_legal_move_list() {
    // Checkmate, stalemate, pins, en passant, checks, promotion, and
    // castling: positions where the pseudo-legal list and the legal list
    // differ, so the existence probe must agree with the full legal list.
    constexpr std::string_view fixtures[] = {
        "4R1k1/6pp/8/8/8/8/8/6K1 b - - 0 1",                            // checkmate
        "k7/8/1Q6/8/8/8/8/K7 b - - 0 1",                                // stalemate
        "4k3/8/8/8/8/4r3/4N3/4K3 w - - 0 1",                            // pinned knight
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2",  // en passant
        "4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 2",                          // pinned en passant
        "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1",                              // in check
        "4k3/8/8/8/1b6/8/8/R3K3 w Q - 0 1",                             // check to be blocked
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",                               // promotion
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",                         // castling
        "r3k2r/ppp2ppp/2n1bn2/3qp3/3P4/2NB1N2/PPP2PPP/R2Q1RK1 w kq - 10 9",  // middlegame
    };
    for (const std::string_view fen : fixtures) {
        const auto parsed = GameState::from_fen(fen);
        require(parsed.has_value(), "legal-move existence fixture FEN must be valid");
        const std::vector<Move> legal = parsed->legal_moves();
        require(parsed->has_legal_move() == !legal.empty(),
                "has_legal_move must agree with the full legal list on fixture FENs");
    }

    // Deterministic seeded playouts: the equality must hold at every visited
    // position, including quiet positions whose first legal candidate sits
    // late in the pseudo-legal list.
    std::mt19937 random(0x5eed1e5u);
    for (int game = 0; game < 4; ++game) {
        GameState state = GameState::startpos();
        for (int ply = 0; ply < 256; ++ply) {
            const std::vector<Move> legal = state.legal_moves();
            require(state.has_legal_move() == !legal.empty(),
                    "has_legal_move must agree with the full legal list at every playout position");
            if (legal.empty() || state.is_draw_by_rule()) {
                break;
            }
            // Plain modulo keeps the playout identical across standard
            // libraries; the distribution bias is irrelevant for coverage.
            require(state.make_move(legal[random() % legal.size()]),
                    "a move from the legal list must be applicable");
        }
    }
}

// ---------------------------------------------------------------------------
// Move-emission order oracle: an independent mailbox generator rebuilt only
// through GameState's public accessors.  It mirrors Position::generate_pseudo's
// emission conditions (piece walk order, ray directions, promotion order,
// castling gates, tactical-only filtering) and re-derives legality by playing
// each candidate on a private board copy.  It shares no code with the native
// generator, so the upcoming bitboard ray-table rewrite must leave
// legal_moves_into and legal_tactical_moves_into element-for-element identical.
// ---------------------------------------------------------------------------

namespace reference {

using koi::Color;
using koi::Move;
using koi::opposite;
using koi::Piece;
using koi::PieceType;
using koi::Promotion;
using koi::Square;

constexpr int kInvalidSquare = 64;

struct Snapshot {
    std::array<Piece, 64> board{};
    Color side = Color::white;
    std::uint8_t castling = 0;
    int en_passant = kInvalidSquare;
    std::array<int, 2> king_square{kInvalidSquare, kInvalidSquare};
};

constexpr Square square_from(int index) noexcept {
    return Square::from_index(static_cast<std::uint8_t>(index));
}

Snapshot capture(const koi::GameState& state) noexcept {
    Snapshot snapshot;
    state.copy_board_to(snapshot.board);
    snapshot.side = state.side_to_move();
    snapshot.castling = state.castling_rights();
    const Square en_passant = state.en_passant_square();
    if (en_passant.index() < Square::kInvalid) {
        snapshot.en_passant = static_cast<int>(en_passant.index());
    }
    for (const Color color : {Color::white, Color::black}) {
        const std::uint64_t king = state.piece_bitboard(PieceType::king, color);
        if (king != 0) {
            snapshot.king_square[color == Color::white ? 0 : 1] =
                static_cast<int>(std::countr_zero(king));
        }
    }
    return snapshot;
}

// Mailbox attack scan rebuilt from the snapshot (the same deliberately
// independent pattern as the SEE test's `reference` namespace).
bool square_attacked(const Snapshot& snapshot, int target, Color attacker) noexcept {
    if (target < 0 || target >= 64) {
        return false;
    }
    const std::array<Piece, 64>& board = snapshot.board;
    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto has_piece = [&board, attacker](int square, PieceType type) noexcept {
        if (square < 0 || square >= 64) {
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

// Position::safe_king_step: move the king onto the transit square with the
// rook still home, then ask whether the transit square is attacked.
bool king_step_is_safe(const Snapshot& snapshot, int from, int to, Color side) noexcept {
    Snapshot trial = snapshot;
    trial.board[static_cast<std::size_t>(to)] = trial.board[static_cast<std::size_t>(from)];
    trial.board[static_cast<std::size_t>(from)] = {};
    return !square_attacked(trial, to, opposite(side));
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
        return PieceType::none;
    }
    return PieceType::none;
}

// Position::push with the same guards: skip the board edge, own pieces, and
// king captures; expand a pawn reaching the promotion rank in queen, rook,
// bishop, knight order.
void emit_push(std::vector<Move>& moves, const Snapshot& snapshot, int from, int to) {
    if (to < 0 || to >= 64) {
        return;
    }
    const Piece target = snapshot.board[static_cast<std::size_t>(to)];
    if (!target.empty() && target.color == snapshot.side) {
        return;
    }
    if (target.type == PieceType::king) {
        return;
    }
    const Piece moving = snapshot.board[static_cast<std::size_t>(from)];
    const int promotion_rank = moving.color == Color::white ? 7 : 0;
    if (moving.type == PieceType::pawn && (to >> 3) == promotion_rank) {
        moves.emplace_back(square_from(from), square_from(to), Promotion::queen);
        moves.emplace_back(square_from(from), square_from(to), Promotion::rook);
        moves.emplace_back(square_from(from), square_from(to), Promotion::bishop);
        moves.emplace_back(square_from(from), square_from(to), Promotion::knight);
        return;
    }
    moves.emplace_back(square_from(from), square_from(to));
}

// Position::generate_pseudo over the snapshot: pieces in ascending square
// order, the production step/ray direction tables verbatim, and castling only
// in the full mode with the king-side gate before the queen-side gate.
void emit_pseudo(const Snapshot& snapshot, bool tactical_only, std::vector<Move>& moves) {
    const Color side = snapshot.side;
    for (int from = 0; from < 64; ++from) {
        const Piece piece = snapshot.board[static_cast<std::size_t>(from)];
        if (piece.empty() || piece.color != side) {
            continue;
        }
        const int file = from & 7;
        const int rank = from >> 3;
        if (piece.type == PieceType::pawn) {
            const int direction = side == Color::white ? 1 : -1;
            const int one = from + direction * 8;
            if (one >= 0 && one < 64) {
                const Piece one_target = snapshot.board[static_cast<std::size_t>(one)];
                if (one_target.empty()) {
                    if (!tactical_only) {
                        emit_push(moves, snapshot, from, one);
                        const int two = from + direction * 16;
                        const int start_rank = side == Color::white ? 1 : 6;
                        if (rank == start_rank &&
                            snapshot.board[static_cast<std::size_t>(two)].empty()) {
                            moves.emplace_back(square_from(from), square_from(two));
                        }
                    } else {
                        const int promotion_rank = side == Color::white ? 7 : 0;
                        if ((one >> 3) == promotion_rank) {
                            emit_push(moves, snapshot, from, one);
                        }
                    }
                }
            }
            for (const int file_delta : {-1, 1}) {
                const int target_file = file + file_delta;
                const int target_rank = rank + direction;
                if (target_file < 0 || target_file >= 8 ||
                    target_rank < 0 || target_rank >= 8) {
                    continue;
                }
                const int to = target_rank * 8 + target_file;
                const Piece target = snapshot.board[static_cast<std::size_t>(to)];
                if ((!target.empty() && target.color != side &&
                     target.type != PieceType::king) ||
                    (snapshot.en_passant == to && target.empty())) {
                    emit_push(moves, snapshot, from, to);
                }
            }
            continue;
        }
        if (piece.type == PieceType::knight || piece.type == PieceType::king) {
            static constexpr std::array<std::array<int, 2>, 8> knight_steps{{
                {{1, 2}}, {{2, 1}}, {{2, -1}}, {{1, -2}},
                {{-1, -2}}, {{-2, -1}}, {{-2, 1}}, {{-1, 2}}}};
            static constexpr std::array<std::array<int, 2>, 8> king_steps{{
                {{1, 1}}, {{1, 0}}, {{1, -1}}, {{0, -1}},
                {{-1, -1}}, {{-1, 0}}, {{-1, 1}}, {{0, 1}}}};
            const auto& steps = piece.type == PieceType::knight ? knight_steps : king_steps;
            for (const auto& step : steps) {
                const int target_file = file + step[0];
                const int target_rank = rank + step[1];
                if (target_file < 0 || target_file >= 8 ||
                    target_rank < 0 || target_rank >= 8) {
                    continue;
                }
                const int to = target_rank * 8 + target_file;
                const Piece target = snapshot.board[static_cast<std::size_t>(to)];
                if (tactical_only) {
                    if (target.empty() || target.color == side ||
                        target.type == PieceType::king) {
                        continue;
                    }
                }
                emit_push(moves, snapshot, from, to);
            }
            if (!tactical_only && piece.type == PieceType::king &&
                !square_attacked(snapshot, from, opposite(side))) {
                const int rank_base = side == Color::white ? 0 : 7;
                if (from == rank_base * 8 + 4) {
                    const std::uint8_t king_right = side == Color::white ?
                        koi::kWhiteKingSideCastling : koi::kBlackKingSideCastling;
                    const std::uint8_t queen_right = side == Color::white ?
                        koi::kWhiteQueenSideCastling : koi::kBlackQueenSideCastling;
                    if ((snapshot.castling & king_right) != 0 &&
                        snapshot.board[static_cast<std::size_t>(rank_base * 8 + 5)].empty() &&
                        snapshot.board[static_cast<std::size_t>(rank_base * 8 + 6)].empty() &&
                        king_step_is_safe(snapshot, from, rank_base * 8 + 5, side) &&
                        king_step_is_safe(snapshot, from, rank_base * 8 + 6, side)) {
                        moves.emplace_back(square_from(from), square_from(rank_base * 8 + 6));
                    }
                    if ((snapshot.castling & queen_right) != 0 &&
                        snapshot.board[static_cast<std::size_t>(rank_base * 8 + 1)].empty() &&
                        snapshot.board[static_cast<std::size_t>(rank_base * 8 + 2)].empty() &&
                        snapshot.board[static_cast<std::size_t>(rank_base * 8 + 3)].empty() &&
                        king_step_is_safe(snapshot, from, rank_base * 8 + 3, side) &&
                        king_step_is_safe(snapshot, from, rank_base * 8 + 2, side)) {
                        moves.emplace_back(square_from(from), square_from(rank_base * 8 + 2));
                    }
                }
            }
            continue;
        }

        const bool diagonal = piece.type == PieceType::bishop || piece.type == PieceType::queen;
        const bool orthogonal = piece.type == PieceType::rook || piece.type == PieceType::queen;
        static constexpr std::array<std::array<int, 2>, 4> diagonal_steps{{
            {{1, 1}}, {{1, -1}}, {{-1, -1}}, {{-1, 1}}}};
        static constexpr std::array<std::array<int, 2>, 4> orthogonal_steps{{
            {{1, 0}}, {{0, 1}}, {{-1, 0}}, {{0, -1}}}};
        const auto add_rays = [&moves, &snapshot, from, file, rank, tactical_only, side](
                                  const auto& steps) {
            for (const auto& step : steps) {
                int target_file = file + step[0];
                int target_rank = rank + step[1];
                while (target_file >= 0 && target_file < 8 &&
                       target_rank >= 0 && target_rank < 8) {
                    const int to = target_rank * 8 + target_file;
                    const Piece target = snapshot.board[static_cast<std::size_t>(to)];
                    if (target.empty()) {
                        if (!tactical_only) {
                            moves.emplace_back(square_from(from), square_from(to));
                        }
                    } else {
                        if (target.color != side && target.type != PieceType::king) {
                            moves.emplace_back(square_from(from), square_from(to));
                        }
                        break;
                    }
                    target_file += step[0];
                    target_rank += step[1];
                }
            }
        };
        if (diagonal) {
            add_rays(diagonal_steps);
        }
        if (orthogonal) {
            add_rays(orthogonal_steps);
        }
    }
}

// Legality by construction: play the candidate on a board copy and ask
// whether the mover's king is attacked.  Castling candidates are already
// fully validated at emission time, exactly like Position::king_move_is_legal.
bool move_is_legal(const Snapshot& snapshot, const Move& move) noexcept {
    const int from = move.from().index();
    const int to = move.to().index();
    if (from >= 64 || to >= 64) {
        return false;
    }
    const Piece moving = snapshot.board[static_cast<std::size_t>(from)];
    if (moving.empty() || moving.color != snapshot.side) {
        return false;
    }
    const int file_delta = (to & 7) - (from & 7);
    if (moving.type == PieceType::king && (file_delta == 2 || file_delta == -2)) {
        return true;
    }
    Snapshot trial = snapshot;
    if (moving.type == PieceType::pawn && to == snapshot.en_passant &&
        trial.board[static_cast<std::size_t>(to)].empty()) {
        trial.board[static_cast<std::size_t>(to + (moving.color == Color::white ? -8 : 8))] = {};
    }
    trial.board[static_cast<std::size_t>(from)] = {};
    Piece placed = moving;
    if (move.promotion() != Promotion::none) {
        placed.type = promotion_piece(move.promotion());
    }
    trial.board[static_cast<std::size_t>(to)] = placed;
    const int king_square = moving.type == PieceType::king ?
        to : snapshot.king_square[moving.color == Color::white ? 0 : 1];
    if (king_square < 0 || king_square >= 64) {
        return false;
    }
    return !square_attacked(trial, king_square, opposite(snapshot.side));
}

std::vector<Move> legal_sequence(const koi::GameState& state, bool tactical_only) {
    const Snapshot snapshot = capture(state);
    // GameState::legal_tactical_moves_with_metadata completes the tactical
    // list to the full legal list while in check (its tactical_only is
    // !checked && !include_quiet_checks), so the oracle mirrors that here.
    const int king_index = snapshot.side == Color::white ? 0 : 1;
    const bool checked =
        snapshot.king_square[king_index] < 64 &&
        square_attacked(snapshot, snapshot.king_square[king_index], opposite(snapshot.side));
    std::vector<Move> pseudo;
    emit_pseudo(snapshot, tactical_only && !checked, pseudo);
    std::vector<Move> legal;
    legal.reserve(pseudo.size());
    for (const Move& move : pseudo) {
        if (move_is_legal(snapshot, move)) {
            legal.push_back(move);
        }
    }
    return legal;
}

// Final legal moves made by a bishop, rook, or queen: the non-vacuity guard
// that a fixture actually exercises long ray walks.
std::size_t count_slider_moves(const koi::GameState& state) {
    const Snapshot snapshot = capture(state);
    std::size_t count = 0;
    for (const Move& move : legal_sequence(state, false)) {
        const Piece piece = snapshot.board[move.from().index()];
        if (piece.type == PieceType::bishop || piece.type == PieceType::rook ||
            piece.type == PieceType::queen) {
            ++count;
        }
    }
    return count;
}

} // namespace reference

void require_same_move_sequence(std::span<const Move> production,
                                const std::vector<Move>& oracle,
                                std::string_view what,
                                const GameState& state) {
    if (production.size() != oracle.size()) {
        throw std::runtime_error(
            std::string(what) + " count mismatch at " + state.fen() +
            ": production=" + std::to_string(production.size()) +
            " oracle=" + std::to_string(oracle.size()));
    }
    for (std::size_t index = 0; index < production.size(); ++index) {
        if (!(production[index] == oracle[index])) {
            throw std::runtime_error(
                std::string(what) + " order mismatch at " + state.fen() +
                " index " + std::to_string(index) +
                ": production=" + production[index].uci() +
                " oracle=" + oracle[index].uci());
        }
    }
}

// Triple differential for one position: the allocation-free full list, the
// vector full list, and the tactical-only list must all match the independent
// oracle exactly, element for element, in sequence order.
void require_emission_order_matches_the_oracle(const GameState& state) {
    const std::vector<Move> oracle_full = reference::legal_sequence(state, false);
    const std::vector<Move> oracle_tactical = reference::legal_sequence(state, true);

    std::array<Move, koi::kMaximumLegalMoves> span_full{};
    const std::size_t span_full_count = state.legal_moves_into(span_full);
    const std::vector<Move> full = state.legal_moves();
    require(span_full_count == full.size(),
            "legal_moves_into and legal_moves must agree on the legal move count");
    require(std::equal(span_full.begin(), span_full.begin() + span_full_count,
                       full.begin(), full.end()),
            "legal_moves_into and legal_moves must expose the same legal move sequence");
    require_same_move_sequence(full, oracle_full, "legal_moves", state);

    // The facade has no span-based tactical entry point; the metadata list
    // with quiet checks and SEE disabled is the capture/promotion contract
    // the oracle mirrors (GameState::legal_tactical_moves_with_metadata).
    koi::MoveMetadataList tactical_metadata;
    (void)state.legal_tactical_moves_with_metadata(
        tactical_metadata, /*include_quiet_checks=*/false, /*include_see=*/false);
    std::vector<Move> tactical;
    tactical.reserve(tactical_metadata.size());
    for (std::size_t index = 0; index < tactical_metadata.size(); ++index) {
        tactical.push_back(tactical_metadata[index].move);
    }
    require_same_move_sequence(tactical, oracle_tactical, "legal_tactical_moves_with_metadata", state);
}

// Shared differential corpus: suite fixtures (startpos, checkmate, stalemate,
// pins, en passant, promotion, castling, in-check replies, an asymmetric
// castling gate), kiwipete for dense ray coverage, all 16 FENs of
// koi-run/sparse-qsearch-fens.txt (embedded; tests never read the file at
// runtime), and the slider-dense corpus position mirrored to the other side
// to move.  The move-emission order oracle and the make/unmake restore
// differential both walk this list.
constexpr std::string_view kDifferentialCorpusFens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b KQkq - 0 1",
        "4R1k1/6pp/8/8/8/8/8/6K1 b - - 0 1",
        "k7/8/1Q6/8/8/8/8/K7 b - - 0 1",
        "4k3/8/8/8/8/4r3/4N3/4K3 w - - 0 1",
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2",
        "4r1k1/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
        "4k3/8/8/8/8/8/4r3/4K3 w - - 0 1",
        "4k3/8/8/8/1b6/8/8/R3K3 w Q - 0 1",
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "4k3/p7/8/8/8/8/8/4K3 b - - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "4k3/8/8/8/8/5r2/8/R3K2R w KQ - 0 1",
        "r3k2r/ppp2ppp/2n1bn2/3qp3/3P4/2NB1N2/PPP2PPP/R2Q1RK1 w kq - 10 9",
        "k7/4r3/5q2/3p4/4P3/2N5/8/7K w - - 0 1",
        "2r1k3/5q1b/8/8/4N3/8/8/K7 w - - 0 1",
        "r3k3/P7/8/8/8/8/7p/1N2K3 b - - 0 1",
        "4k2r/P7/8/8/8/8/7p/4K3 w - - 0 1",
        "kn2q3/8/8/8/8/8/P7/4K3 w - - 0 1",
        "8/8/4r1q1/7k/8/7N/P7/K7 w - - 0 1",
        "k7/4P3/8/8/8/8/p7/4R1K1 w - - 0 1",
        "4k3/1P6/8/8/8/8/6p1/4K1N1 w - - 0 1",
        "b7/8/3q3k/8/7r/6N1/8/K7 w - - 0 1",
        "8/3q1r2/2k5/8/8/5N2/8/K1B5 w - - 0 1",
        "4k3/8/3q4/4r3/8/8/1B6/4K3 w - - 0 1",
        "4k3/1P6/8/8/8/8/6p1/2B1K3 b - - 0 1",
        "4k3/3q4/8/8/8/8/3Q4/3R2K1 w - - 0 1",
        "2b1k3/7P/8/8/8/8/3p4/5K1R b - - 0 1",
        "4k1r1/8/8/3Q4/2q3N1/8/4B1B1/4R1K1 b - - 0 1",
        "4k3/8/8/8/2rQ4/8/8/3R2K1 b - - 0 1",
        "4k1r1/8/8/3Q4/2q3N1/8/4B1B1/4R1K1 w - - 0 1",
};

void test_move_emission_order_matches_the_reference_oracle() {
    for (const std::string_view fen : kDifferentialCorpusFens) {
        const auto parsed = GameState::from_fen(fen);
        require(parsed.has_value(), "emission-order fixture FEN must be valid");
        require_emission_order_matches_the_oracle(*parsed);
    }

    // Non-vacuity guard: the slider-dense corpus position must genuinely walk
    // long rays, otherwise the sequence comparison would be trivial.
    const auto slider_dense =
        GameState::from_fen("4k1r1/8/8/3Q4/2q3N1/8/4B1B1/4R1K1 b - - 0 1");
    require(slider_dense.has_value(), "slider-dense fixture must be valid");
    require(reference::count_slider_moves(*slider_dense) >= 15,
            "the slider-dense position must emit a long slider ray sequence");
    require(slider_dense->legal_moves().size() >= 20,
            "the slider-dense position must emit a long legal move list");
    const auto kiwipete = GameState::from_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    require(kiwipete.has_value() && kiwipete->legal_moves().size() >= 40,
            "kiwipete must emit a long legal move list");

    // Deterministic seeded playouts: the element-for-element identity must
    // hold at every visited position, including promotions, en passant,
    // castling, and discovered checks.
    std::mt19937 random(0x5eed1e5u);
    for (int game = 0; game < 4; ++game) {
        GameState state = GameState::startpos();
        for (int ply = 0; ply < 128; ++ply) {
            require_emission_order_matches_the_oracle(state);
            const std::vector<Move> legal = state.legal_moves();
            if (legal.empty() || state.is_draw_by_rule()) {
                break;
            }
            require(state.make_move(legal[random() % legal.size()]),
                    "a move from the legal list must be applicable");
        }
    }
}

// ---------------------------------------------------------------------------
// Make/unmake restore differential: search applies moves through
// GameState::make_search_move (declared game_state.hpp:322, defined at
// game_state.cpp:1927) and restores them through GameState::unmake_move
// (declared game_state.hpp:329, defined at game_state.cpp:1954) - the exact
// pair behind detail::SearchContext::make_observed / unmake_observed
// (search_context.hpp:531 and :540).  Every observable of the parent position
// must be bit-identical after make -> unmake, and the incremental key right
// after each make must equal the key of a fresh parse of the resulting FEN.
// This is the standing arbiter for any future inverse-unmake or snapshot
// change.
// ---------------------------------------------------------------------------

struct RestoreObservables {
    std::string fen;
    std::uint64_t position_key = 0;
    std::vector<Move> legal_moves;
    std::array<koi::Piece, 64> board{};
    koi::Color side_to_move = koi::Color::white;
    std::uint8_t castling_rights = 0;
    Square en_passant{};
    std::uint16_t halfmove_clock = 0;
    std::uint16_t fullmove_number = 0;
};

RestoreObservables capture_restore_observables(const GameState& state) {
    RestoreObservables captured;
    captured.fen = state.fen();
    captured.position_key = state.position_key();
    captured.legal_moves = state.legal_moves();
    state.copy_board_to(captured.board);
    captured.side_to_move = state.side_to_move();
    captured.castling_rights = state.castling_rights();
    captured.en_passant = state.en_passant_square();
    captured.halfmove_clock = state.halfmove_clock();
    captured.fullmove_number = state.fullmove_number();
    return captured;
}

// The incremental key immediately after a make must agree with a fresh parse
// of the resulting FEN; catches incremental-key drift that FEN comparison
// alone cannot see (the same pattern as the null-move key test above).
void require_key_matches_fresh_parse(const GameState& state, const std::string& context) {
    const std::string fen = state.fen();
    const auto fresh = GameState::from_fen(fen);
    if (!fresh.has_value()) {
        throw std::runtime_error("post-make FEN must re-parse" + context + ": " + fen);
    }
    if (fresh->position_key() != state.position_key()) {
        throw std::runtime_error(
            "incremental position key must match a fresh parse" + context +
            ": incremental=" + std::to_string(state.position_key()) +
            " fresh=" + std::to_string(fresh->position_key()) + " fen=" + fen);
    }
}

void require_restore_observables(const RestoreObservables& expected, const GameState& state,
                                 const std::string& context) {
    require(state.fen() == expected.fen, "unmake must restore the FEN" + context);
    require(state.position_key() == expected.position_key,
            "unmake must restore the position key" + context);
    const std::vector<Move> legal = state.legal_moves();
    require_same_move_sequence(legal, expected.legal_moves, "unmake legal move list", state);
    std::array<koi::Piece, 64> board{};
    state.copy_board_to(board);
    for (std::size_t index = 0; index < board.size(); ++index) {
        if (board[index].type != expected.board[index].type ||
            board[index].color != expected.board[index].color) {
            throw std::runtime_error("unmake must restore the mailbox" + context +
                                     " square " + std::to_string(index));
        }
    }
    require(state.side_to_move() == expected.side_to_move,
            "unmake must restore the side to move" + context);
    require(state.castling_rights() == expected.castling_rights,
            "unmake must restore the castling rights" + context);
    require(state.en_passant_square() == expected.en_passant,
            "unmake must restore the en passant square" + context);
    require(state.halfmove_clock() == expected.halfmove_clock,
            "unmake must restore the halfmove clock" + context);
    require(state.fullmove_number() == expected.fullmove_number,
            "unmake must restore the fullmove number" + context);
}

// One full make/unmake/assert cycle for a generated move of the current
// position; the caller guarantees `metadata` was generated for this state.
void require_restore_cycle(GameState& state, const koi::MoveMetadata& metadata,
                           const std::string& context, std::size_t& move_cycles) {
    const RestoreObservables baseline = capture_restore_observables(state);
    require(state.make_search_move(metadata), "make_search_move must apply" + context);
    require_key_matches_fresh_parse(state, context + " after make");
    require(state.unmake_move(), "a made search move must be undoable" + context);
    require_restore_observables(baseline, state, context);
    ++move_cycles;
}

// Every legal move of one position must round-trip.  The metadata list covers
// the complete legal list, so iterating it is exactly "for every legal move".
void require_every_move_restores(GameState& state, const std::string& context,
                                 std::size_t& move_cycles) {
    koi::MoveMetadataList metadata;
    state.legal_moves_with_metadata(metadata);
    require(metadata.size() == state.legal_moves().size(),
            "the metadata batch must cover every legal move" + context);
    for (const koi::MoveMetadata& candidate : metadata) {
        require_restore_cycle(state, candidate,
                              context + " move " + candidate.move.uci(), move_cycles);
    }
}

void test_make_unmake_restore_differential() {
    std::size_t move_cycles = 0;

    // (a) Every legal move of every corpus fixture, on both a mirror-attached
    // public state and a mirror-detached state (the search configuration:
    // search_runner.cpp detaches its root mirror before searching).
    for (const std::string_view fen : kDifferentialCorpusFens) {
        const auto parsed = GameState::from_fen(fen);
        require(parsed.has_value(), "restore-differential fixture FEN must be valid");
        const std::string context = " at " + std::string(fen);

        GameState attached = *parsed;
        require_every_move_restores(attached, context, move_cycles);

        GameState detached = *parsed;
        detached.detach_mirror();
        require_every_move_restores(detached, context + " (detached mirror)", move_cycles);
    }

    // (b) Deterministic seeded playouts: at every node the key must match a
    // fresh parse, the make/unmake cycle must restore every observable, and
    // the restored position must accept the chosen move again to advance.
    std::mt19937 random(0x5eed2e5u);
    for (int game = 0; game < 4; ++game) {
        GameState state = GameState::startpos();
        state.detach_mirror();
        for (int ply = 0; ply < 128; ++ply) {
            const std::string context = " in playout " + std::to_string(game) +
                " ply " + std::to_string(ply);
            require_key_matches_fresh_parse(state, context);
            koi::MoveMetadataList metadata;
            state.legal_moves_with_metadata(metadata);
            if (metadata.empty() || state.is_draw_by_rule()) {
                break;
            }
            // Plain modulo keeps the playout identical across standard
            // libraries; the distribution bias is irrelevant for coverage.
            const koi::MoveMetadata& chosen = metadata[random() % metadata.size()];
            require_restore_cycle(state, chosen, context + " move " + chosen.move.uci(),
                                  move_cycles);
            require(state.make_search_move(chosen),
                    "the restored position must accept the chosen move again" + context);
        }
    }

    // Non-vacuity guard: the corpus and playouts together must produce far
    // more than a token number of cycles, so a silent coverage regression
    // fails loudly instead of passing on an empty walk.
    require(move_cycles > 500,
            "the restore differential must exercise more than 500 move cycles");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"Koi-owned move coordinates", test_move_uses_koi_coordinates_and_formats_uci},
        {"FEN construction", test_fen_constructs_a_game_state},
        {"strict FEN legality", test_strict_fen_rejects_impossible_material_and_check_state},
        {"legal special moves", test_game_state_exposes_legal_special_moves},
        {"special move conversion round trips", test_special_move_conversion_round_trips_without_uci_translation},
        {"move metadata", test_move_metadata_identifies_special_moves_and_checks},
        {"batch move metadata", test_batch_move_metadata_matches_individual_descriptions},
        {"search metadata fast path", test_search_metadata_path_can_skip_check_analysis},
        {"generated move application", test_generated_move_application_preserves_special_move_semantics},
        {"all fast generated moves", test_every_generated_move_can_use_the_fast_application_path},
        {"draw rule view", test_draw_rule_view_separates_draws_from_move_exhaustion},
        {"non-pawn material view", test_non_pawn_material_view_is_available_for_search_safety_checks},
        {"tactical move generation", test_tactical_move_generation_excludes_quiet_moves_but_keeps_checks},
        {"position features", test_position_features_expose_symmetric_board_attacks_and_mobility},
        {"make/unmake restoration", test_make_and_unmake_restore_fen_and_key},
        {"null move restoration", test_null_move_round_trips_side_fen_and_key},
        {"null move incremental key", test_null_move_incremental_key_matches_a_fresh_parse},
        {"transactional rejected move", test_rejected_king_move_leaves_state_unchanged},
        {"start-position move count", test_start_position_has_twenty_legal_moves},
        {"legal-move existence differential", test_legal_move_existence_matches_the_legal_move_list},
        {"move emission order differential", test_move_emission_order_matches_the_reference_oracle},
        {"make unmake restore differential", test_make_unmake_restore_differential},
    };

    return koi::test::run_tests(tests, argc, argv);
}
