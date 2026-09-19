// Focused unit coverage for the classical evaluator: perspective handling,
// breakdown bookkeeping, material counting, mobility sign, the phase gate on
// tempo and insufficient-material draws.  Search-level score gates stay in
// koi_search_tests; this suite pins the evaluator contract directly.

#include <filesystem>
#include <fstream>
#include <string>
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

// Mirrors the evaluator's round-to-nearest fixed-point application so the
// tests pin the rounding contract, not just the scale factor.
[[nodiscard]] int scaled_total(const EvaluationBreakdown& breakdown) {
    const int half = koi::kEvaluationScaleOne / 2;
    const int sum = breakdown_sum(breakdown);
    const int adjustment = sum >= 0 ? half : -half;
    return (sum * breakdown.endgame_scale + adjustment) / koi::kEvaluationScaleOne;
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
        require(scaled_total(breakdown) == breakdown.total,
                "the named terms must sum to the reported total up to the endgame scale");
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

void test_endgame_scale_discounts_drawn_minor_endings() {
    const koi::ClassicalEvaluator evaluator;
    // Bishop versus knight without pawns is a dead draw in practice.
    const GameState bishop_vs_knight = state_from("4k3/8/5n2/8/8/8/8/K1B5 w - - 0 1");
    const EvaluationBreakdown minor = evaluator.breakdown(bishop_vs_knight, Color::white);
    require(minor.endgame_scale == evaluator.parameters().endgame_scale_minor_only,
            "pawnless bishop-versus-knight must receive the minor-piece draw scale");
    require(minor.total == scaled_total(minor),
            "the scaled total must stay the fixed-point product of the terms");

    // Two minors on one side (KBN versus K) keep their winning value.
    const GameState two_minors = state_from("4k3/8/8/8/8/8/8/KB5N w - - 0 1");
    require(evaluator.breakdown(two_minors, Color::white).endgame_scale ==
                koi::kEvaluationScaleOne,
            "two minors versus a bare king must keep the full evaluation");
}

void test_endgame_scale_halves_opposite_colored_bishops() {
    const koi::ClassicalEvaluator evaluator;
    // c1 (dark) versus c8 (light) with one pawn: the standard drawn OCB ending.
    const GameState opposite = state_from("2b1k3/8/8/8/8/8/4P3/2B1K3 w - - 0 1");
    const EvaluationBreakdown ocb = evaluator.breakdown(opposite, Color::white);
    require(ocb.endgame_scale == evaluator.parameters().endgame_scale_opposite_bishops,
            "opposite-colored bishops must receive the drawing scale");
    require(ocb.total == scaled_total(ocb),
            "the scaled total must stay the fixed-point product of the terms");

    // Same-colored bishops are not the drawn pattern and stay untouched.
    const GameState same = state_from("3bk3/8/8/8/8/8/4P3/2B1K3 w - - 0 1");
    require(evaluator.breakdown(same, Color::white).endgame_scale == koi::kEvaluationScaleOne,
            "same-colored bishops must keep the full evaluation");
}

void test_king_activity_tapers_beyond_the_endgame_threshold() {
    const koi::ClassicalEvaluator evaluator;
    // Queen ending (phase 4): the term is active, so a centralized king must
    // outscore a cornered one even though the old hard phase gate zeroed both.
    const GameState central = state_from("k7/8/8/8/3K4/8/8/3Q4 w - - 0 1");
    const GameState corner = state_from("k7/8/8/8/8/8/8/K2Q4 w - - 0 1");
    require(evaluator.breakdown(central, Color::white).king_activity >
                evaluator.breakdown(corner, Color::white).king_activity,
            "king activity must fade in before the bare-kings phase");
    require(evaluator.breakdown(corner, Color::white).king_activity == 0,
            "a cornered king must not receive activity credit");

    const GameState opening = state_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    require(evaluator.breakdown(opening, Color::white).king_activity == 0,
            "king activity must stay out of the full-material middlegame");
}

void test_endgame_fixture_invariants() {
    const koi::ClassicalEvaluator evaluator;
    const std::filesystem::path fixture = koi::test::fixture_path("endgames/endgame-positions.txt");
    std::ifstream stream(fixture);
    require(stream.good(), "the endgame fixture must open");

    std::size_t checked = 0;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        const std::size_t separator = line.find('|');
        require(separator != std::string::npos, "endgame fixture lines must be name|FEN");
        const GameState state = state_from(
            std::string_view(line).substr(separator + 1));

        const EvaluationBreakdown white = evaluator.breakdown(state, Color::white);
        const EvaluationBreakdown black = evaluator.breakdown(state, Color::black);
        require(white.total == -black.total,
                "endgame fixture totals must negate across perspectives");
        require(white.endgame_scale == black.endgame_scale,
                "both perspectives must select the same endgame scale");
        if (state.is_dead_position()) {
            require(white.total == 0 && black.total == 0,
                    "dead endgame fixtures must score exactly zero");
        } else {
            require(scaled_total(white) == white.total,
                    "endgame totals must follow the fixed-point scale rounding");
        }
        require(white.endgame_scale == koi::kEvaluationScaleOne ||
                    white.endgame_scale == evaluator.parameters().endgame_scale_minor_only ||
                    white.endgame_scale == evaluator.parameters().endgame_scale_opposite_bishops,
                "endgame fixture scales must come from the parameter table");
        ++checked;
    }
    require(checked >= 16, "the endgame fixture must cover the curated corpus");
}

void test_endgame_king_proximity_shapes_passed_pawn_value() {
    const koi::ClassicalEvaluator evaluator;
    // Same pawn, same white king; only the hostile king differs.  With the
    // black king on c7 the d5 passer is within two squares of it, so the
    // enemy-king penalty must discount the pawn.
    const GameState hostile_king_near = state_from("8/2k5/8/3P4/8/8/8/K7 w - - 0 1");
    const GameState hostile_king_far = state_from("7k/8/8/3P4/8/8/8/K7 w - - 0 1");
    require(evaluator.breakdown(hostile_king_near, Color::white).passed_pawn <
                evaluator.breakdown(hostile_king_far, Color::white).passed_pawn,
            "a passed pawn the enemy king controls must be worth less");

    // Owning-king support must still dominate: the d4 king supports the d5
    // passer while the a1 king cannot reach it.
    const GameState supported = state_from("7k/8/8/3P4/3K4/8/8/8 w - - 0 1");
    const GameState unsupported = state_from("7k/8/8/3P4/8/8/8/K7 w - - 0 1");
    require(evaluator.breakdown(supported, Color::white).passed_pawn >
                evaluator.breakdown(unsupported, Color::white).passed_pawn,
            "an own king supporting the passer must outscore a distant one");
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
        {"classical evaluator minor-piece draw scale", test_endgame_scale_discounts_drawn_minor_endings},
        {"classical evaluator opposite-bishop scale", test_endgame_scale_halves_opposite_colored_bishops},
        {"classical evaluator king activity taper", test_king_activity_tapers_beyond_the_endgame_threshold},
        {"classical evaluator passed pawn king proximity", test_endgame_king_proximity_shapes_passed_pawn_value},
        {"classical evaluator endgame fixture invariants", test_endgame_fixture_invariants},
    };
    return koi::test::run_tests(tests, argc, argv);
}
