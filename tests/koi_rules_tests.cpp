#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"

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

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
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
        {"transactional rejected move", test_rejected_king_move_leaves_state_unchanged},
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
