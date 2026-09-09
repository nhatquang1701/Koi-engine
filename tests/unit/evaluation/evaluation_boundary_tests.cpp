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
}

} // namespace

int main() {
    try {
        test_castling_rights_improve_development_readiness();
        std::cout << "PASS castling readiness\n";
        test_pawn_break_potential_improves_center_term();
        std::cout << "PASS pawn break potential\n";
        test_compiled_parameter_metadata_is_versioned();
        std::cout << "PASS parameter metadata\n";
        test_trapped_piece_is_penalized_in_initiative();
        std::cout << "PASS trapped piece\n";
        test_hanging_piece_is_penalized_in_initiative();
        std::cout << "PASS hanging piece\n";
        test_feature_extractor_preserves_cached_king_and_pawn_context();
        std::cout << "PASS feature extraction context\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
