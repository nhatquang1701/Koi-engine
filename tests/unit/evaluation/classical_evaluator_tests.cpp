// Focused unit coverage for the classical evaluator: perspective handling,
// breakdown bookkeeping, material counting, mobility sign, the phase gate on
// tempo and insufficient-material draws.  Search-level score gates stay in
// koi_search_tests; this suite pins the evaluator contract directly.

#include <string_view>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi_test_support.hpp"

namespace {

using koi::Color;
using koi::EvaluationBreakdown;
using koi::GameState;
using koi::test::require;

[[nodiscard]] GameState state_from(std::string_view fen) {
    return koi::test::require_value(GameState::from_fen(fen), "fixture FEN must parse");
}

[[nodiscard]] int breakdown_sum(const EvaluationBreakdown& breakdown) {
    return breakdown.material + breakdown.piece_square + breakdown.mobility +
        breakdown.pawn_structure + breakdown.activity + breakdown.development +
        breakdown.center_control + breakdown.initiative + breakdown.king_safety +
        breakdown.king_activity + breakdown.passed_pawn + breakdown.tempo;
}

const std::vector<std::string_view> kLiveFens = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1",
};

void test_perspective_negates_the_score() {
    const koi::ClassicalEvaluator evaluator;
    for (const std::string_view fen : kLiveFens) {
        const GameState state = state_from(fen);
        require(evaluator.evaluate(state, Color::white) ==
                    -evaluator.evaluate(state, Color::black),
                "the white and black perspectives must negate each other");
        require(evaluator.breakdown(state, Color::white).total ==
                    -evaluator.breakdown(state, Color::black).total,
                "the breakdown totals must negate each other");
    }
}

void test_breakdown_terms_sum_to_total() {
    const koi::ClassicalEvaluator evaluator;
    for (const std::string_view fen : kLiveFens) {
        const GameState state = state_from(fen);
        const EvaluationBreakdown breakdown = evaluator.breakdown(state, Color::white);
        require(breakdown_sum(breakdown) == breakdown.total,
                "the named terms must sum to the reported total");
        require(evaluator.evaluate(state, Color::white) == breakdown.total,
                "evaluate must return the breakdown total");
    }
}

void test_symmetric_positions_score_even() {
    const koi::ClassicalEvaluator evaluator;
    const GameState start = state_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    require(evaluator.evaluate(start, Color::white) == 0,
            "the symmetric starting position must evaluate to zero");
}

void test_insufficient_material_scores_zero() {
    const koi::ClassicalEvaluator evaluator;
    const std::vector<std::string_view> dead_fens = {
        "8/8/8/8/8/8/8/K6k w - - 0 1",
        "8/8/8/8/8/8/8/KN5k w - - 0 1",
        "8/8/8/8/8/8/8/KB5k w - - 0 1",
    };
    for (const std::string_view fen : dead_fens) {
        const GameState state = state_from(fen);
        const EvaluationBreakdown breakdown = evaluator.breakdown(state, Color::white);
        require(breakdown.total == 0, "insufficient material must evaluate as a draw");
    }
    // The individual terms stay visible for diagnostics; only the total is
    // forced to a draw.  Bare kings have no material to report at all.
    require(evaluator.breakdown(state_from("8/8/8/8/8/8/8/K6k w - - 0 1"), Color::white).material == 0,
            "bare kings must not report material");
}

void test_locked_pawn_wall_scores_zero() {
    // The native position owns dead-position recognition.  This blocked pawn
    // wall is the known FIDE example the evaluator now defers to, so the total
    // must be a draw even though both sides have pawns and bishops.
    const koi::ClassicalEvaluator evaluator;
    const GameState state = state_from("8/2b1k3/7p/p1p1p2P/PpP1P3/1P1BK3/8/8 w - - 0 1");
    require(state.is_dead_position(), "the fixture must be a recognized dead position");
    const EvaluationBreakdown breakdown = evaluator.breakdown(state, Color::white);
    require(breakdown.total == 0, "a locked pawn wall must evaluate as a draw");
    require(breakdown.passed_pawn != 0 || breakdown.pawn_structure != 0,
            "diagnostic terms stay visible for a locked wall");
}

void test_material_advantage_is_counted() {
    const koi::ClassicalEvaluator evaluator;
    const GameState queen_up = state_from("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    require(evaluator.breakdown(queen_up, Color::white).material == 900,
            "an extra queen must add 900 centipawns of material");

    const GameState rook_up = state_from("4k3/8/8/8/8/8/8/3RK3 w - - 0 1");
    require(evaluator.breakdown(rook_up, Color::white).material == 500,
            "an extra rook must add 500 centipawns of material");
}

void test_mobility_term_follows_the_extra_rook() {
    const koi::ClassicalEvaluator evaluator;
    const GameState state = state_from("4k3/8/8/8/8/8/8/4K2R w - - 0 1");
    require(evaluator.breakdown(state, Color::white).mobility > 0,
            "the side with the only rook must have positive mobility");
    require(evaluator.breakdown(state, Color::black).mobility < 0,
            "the side without the rook must have negative mobility");
}

void test_endgame_tempo_is_phase_gated() {
    const koi::ClassicalEvaluator evaluator;
    const int bonus = evaluator.parameters().tempo_bonus;
    require(bonus > 0, "the fixture expects a positive tempo bonus");

    const GameState white_to_move = state_from("4k3/8/8/8/8/8/8/4K2R w - - 0 1");
    const GameState black_to_move = state_from("4k3/8/8/8/8/8/8/4K2R b - - 0 1");
    require(evaluator.breakdown(white_to_move, Color::white).tempo == bonus,
            "the side to move must receive the endgame tempo bonus");
    require(evaluator.breakdown(black_to_move, Color::white).tempo == -bonus,
            "tempo must follow the side to move, not the perspective");

    const GameState opening = state_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    require(evaluator.breakdown(opening, Color::white).tempo == 0,
            "tempo must stay out of the opening phase");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests = {
        {"classical evaluator perspective", test_perspective_negates_the_score},
        {"classical evaluator breakdown sum", test_breakdown_terms_sum_to_total},
        {"classical evaluator symmetric start", test_symmetric_positions_score_even},
        {"classical evaluator insufficient material", test_insufficient_material_scores_zero},
        {"classical evaluator locked pawn wall", test_locked_pawn_wall_scores_zero},
        {"classical evaluator material", test_material_advantage_is_counted},
        {"classical evaluator mobility", test_mobility_term_follows_the_extra_rook},
        {"classical evaluator tempo gate", test_endgame_tempo_is_phase_gated},
    };
    return koi::test::run_tests(tests, argc, argv);
}
