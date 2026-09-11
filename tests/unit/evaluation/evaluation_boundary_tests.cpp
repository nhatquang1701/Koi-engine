#include <iostream>
#include <array>
#include <string>

#include "koi/classical_evaluator.hpp"
#include "koi/evaluation_features.hpp"
#include "koi/evaluation_parameters_generated.hpp"
#include "koi/game_state.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "evaluation fixture must be valid: " + std::string(fen));
    return *state;
}

void test_castling_rights_improve_development_readiness() {
    const koi::GameState ready = require_state(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQ - 0 1");
    const koi::GameState unavailable = require_state(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    const koi::ClassicalEvaluator evaluator;

    const auto ready_score = evaluator.breakdown(ready, koi::Color::white);
    const auto unavailable_score = evaluator.breakdown(unavailable, koi::Color::white);
    require(ready_score.development > unavailable_score.development,
            "available castling rights must improve development readiness");
}

void test_early_queen_development_is_tempered() {
    const koi::GameState queen_early = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/Q1PP4/2N5/PP2PPPP/R1B1KBNR b KQkq - 1 4");
    const koi::GameState queen_home = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq - 1 4");
    const koi::ClassicalEvaluator evaluator;

    const auto early_score = evaluator.breakdown(queen_early, koi::Color::white);
    const auto home_score = evaluator.breakdown(queen_home, koi::Color::white);
    require(home_score.development > early_score.development,
            "an early queen sortie before minor-piece development must lose development credit");
}

void test_central_early_queen_development_is_tempered() {
    const koi::GameState queen_central = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N2Q2/PP2PPPP/R1B1KBNR b KQkq - 1 4");
    const koi::GameState queen_home = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq - 1 4");
    const koi::ClassicalEvaluator evaluator;

    const auto central_score = evaluator.breakdown(queen_central, koi::Color::white);
    const auto home_score = evaluator.breakdown(queen_home, koi::Color::white);
    require(home_score.development > central_score.development,
            "a central queen sortie before minor-piece development must lose development credit");
}

void test_late_central_queen_is_not_charged_opening_penalty() {
    const koi::GameState queen_central = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N2Q2/PP2PPPP/R1B1KBNR b KQkq - 1 7");
    const koi::GameState queen_home = require_state(
        "rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq - 1 7");
    const koi::ClassicalEvaluator evaluator;

    require(evaluator.breakdown(queen_central, koi::Color::white).development ==
                evaluator.breakdown(queen_home, koi::Color::white).development,
            "the opening queen-development penalty must taper out after the opening window");
}

void test_pawn_break_potential_improves_center_term() {
    const koi::GameState break_position = require_state(
        "r3k2r/8/8/3p4/2P1P3/8/8/R3K2R w - - 0 1");
    const koi::GameState quiet_position = require_state(
        "r3k2r/8/3p4/8/2P1P3/8/8/R3K2R w - - 0 1");
    const koi::ClassicalEvaluator evaluator;

    const auto break_score = evaluator.breakdown(break_position, koi::Color::white);
    const auto quiet_score = evaluator.breakdown(quiet_position, koi::Color::white);
    require(break_score.center_control > quiet_score.center_control,
            "a pawn with an immediate central break must improve center potential");
}

void test_compiled_parameter_metadata_is_versioned() {
    const koi::ClassicalEvaluator evaluator;
    const auto& metadata = koi::kClassicalEvaluationParameterMetadata;
    require(!metadata.format_version.empty() && !metadata.parameter_version.empty(),
            "compiled evaluation metadata must expose format and parameter versions");
    require(metadata.parameter_version == evaluator.parameters().version,
            "compiled metadata must identify the active classical parameter version");
}

void test_trapped_piece_is_penalized_in_initiative() {
    const koi::GameState trapped = require_state(
        "4k3/8/8/8/8/1P6/2P5/N3K3 w - - 0 1");
    const koi::GameState active = require_state(
        "4k3/8/8/1P6/8/2N5/2P5/4K3 w - - 0 1");
    const koi::ClassicalEvaluator evaluator;

    const auto trapped_score = evaluator.breakdown(trapped, koi::Color::white);
    const auto active_score = evaluator.breakdown(active, koi::Color::white);
    require(trapped_score.initiative < active_score.initiative,
            "a fully blocked minor piece must receive a trapped-piece initiative penalty");
}

void test_hanging_piece_is_penalized_in_initiative() {
    const koi::GameState hanging = require_state(
        "4k3/8/8/8/8/8/5n2/4K2R w - - 0 1");
    const koi::GameState safe = require_state(
        "n3k3/8/8/8/8/8/8/4K2R w - - 0 1");
    const koi::ClassicalEvaluator evaluator;

    const auto hanging_score = evaluator.breakdown(hanging, koi::Color::white);
    const auto safe_score = evaluator.breakdown(safe, koi::Color::white);
    require(hanging_score.initiative < safe_score.initiative,
            "an undefended attacked piece must receive a hanging-piece penalty");
}

void test_major_attackers_are_more_dangerous_in_king_ring() {
    // Both positions have the same phase and the same number of attacked
    // king-ring squares.  The first position attacks the ring with a queen;
    // the comparison position uses a bishop, with a rook and knight placed
    // away from the king to keep the phase equal.
    const koi::GameState queen_attack = require_state(
        "4k3/8/8/8/4q3/8/5PPP/RNB3K1 w - - 0 11");
    const koi::GameState minor_attack = require_state(
        "rn2k3/8/8/8/4b3/8/5PPP/RNB3K1 w - - 0 11");
    const koi::ClassicalEvaluator evaluator;

    const auto queen_score = evaluator.breakdown(queen_attack, koi::Color::white);
    const auto minor_score = evaluator.breakdown(minor_attack, koi::Color::white);
    require(queen_score.king_safety < minor_score.king_safety,
            "a major attacker in the king ring must be more dangerous than a minor attacker");
}

void test_castle_ready_king_ring_pressure_is_deferred() {
    const koi::GameState castle_ready = require_state(
        "rn2k2r/8/8/1q6/8/8/8/RN2K2R w KQkq - 0 11");
    const koi::GameState castle_unavailable = require_state(
        "rn2k2r/8/8/1q6/8/8/8/RN2K2R w - - 0 11");
    const koi::ClassicalEvaluator evaluator;

    const auto ready_features = koi::EvaluationFeatureExtractor::extract(castle_ready);
    const auto unavailable_features = koi::EvaluationFeatureExtractor::extract(castle_unavailable);
    require(ready_features.castling_rights == koi::kAllCastlingRights &&
                unavailable_features.castling_rights == 0,
            "castle-ready ring fixture must preserve castling rights in evaluation features");

    require(evaluator.breakdown(castle_ready, koi::Color::white).king_safety >
                evaluator.breakdown(castle_unavailable, koi::Color::white).king_safety,
            "king-ring pressure must be deferred while the king can still castle");
}

void test_sparse_endgame_king_ring_pressure_is_tapered() {
    const koi::GameState ring_attack = require_state(
        "4k3/8/8/8/4Q3/8/8/6K1 b - - 0 1");
    const koi::GameState quiet_queen = require_state(
        "4k3/8/8/8/Q7/8/8/6K1 b - - 0 1");
    const koi::ClassicalEvaluator evaluator;

    require(evaluator.breakdown(ring_attack, koi::Color::white).king_safety ==
                evaluator.breakdown(quiet_queen, koi::Color::white).king_safety,
            "sparse queen endgames must not let a king-ring attacker override endgame scaling");
}

void test_feature_extractor_preserves_cached_king_and_pawn_context() {
    const koi::GameState state = require_state(
        "4k3/7p/8/8/8/P7/P7/4K3 w - - 0 1");
    const koi::EvaluationFeatures extracted =
        koi::EvaluationFeatureExtractor::extract(state);
    require(extracted.position.king_squares[0].index() == 4 &&
                extracted.position.king_squares[1].index() == 60,
            "evaluation extraction must preserve both cached king squares");
    require((extracted.position.pawn_file_masks[0] & 0x01U) != 0 &&
                (extracted.position.pawn_file_masks[1] & 0x80U) != 0,
            "evaluation extraction must preserve white a-file and black h-file pawns");
    require(extracted.position.castling_rights == state.castling_rights(),
            "cached evaluation features must preserve castling rights");
}

} // namespace

int main() {
    try {
        test_castling_rights_improve_development_readiness();
        std::cout << "PASS castling readiness\n";
        test_early_queen_development_is_tempered();
        std::cout << "PASS early queen development\n";
        test_central_early_queen_development_is_tempered();
        std::cout << "PASS central early queen development\n";
        test_late_central_queen_is_not_charged_opening_penalty();
        std::cout << "PASS late central queen taper\n";
        test_pawn_break_potential_improves_center_term();
        std::cout << "PASS pawn break potential\n";
        test_compiled_parameter_metadata_is_versioned();
        std::cout << "PASS parameter metadata\n";
        test_trapped_piece_is_penalized_in_initiative();
        std::cout << "PASS trapped piece\n";
        test_hanging_piece_is_penalized_in_initiative();
        std::cout << "PASS hanging piece\n";
        test_major_attackers_are_more_dangerous_in_king_ring();
        std::cout << "PASS weighted king-ring attackers\n";
        test_castle_ready_king_ring_pressure_is_deferred();
        std::cout << "PASS castle-ready king-ring deferral\n";
        test_sparse_endgame_king_ring_pressure_is_tapered();
        std::cout << "PASS sparse king-ring taper\n";
        test_feature_extractor_preserves_cached_king_and_pawn_context();
        std::cout << "PASS feature extraction context\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
