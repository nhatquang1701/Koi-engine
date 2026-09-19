#pragma once

#include "koi/evaluator.hpp"
#include "koi/evaluation_parameters.hpp"

namespace koi {

// Fixed-point base for EvaluationBreakdown::endgame_scale.  A scale of 64
// leaves the score untouched; smaller factors discount the drawish endings.
inline constexpr int kEvaluationScaleOne = 64;

struct EvaluationBreakdown {
    int material = 0;
    int piece_square = 0;
    int mobility = 0;
    int pawn_structure = 0;
    int activity = 0;
    int development = 0;
    int center_control = 0;
    int initiative = 0;
    int king_safety = 0;
    int king_activity = 0;
    int passed_pawn = 0;
    int tempo = 0;
    // Drawishness factor applied to the summed terms, in 1/64 units.  It is a
    // magnitude, not a signed score: perspective negation leaves it unchanged.
    // `total` therefore equals sum(terms) * endgame_scale / kEvaluationScaleOne
    // for live positions.
    int endgame_scale = kEvaluationScaleOne;
    int total = 0;
};

class ClassicalEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState&, Color perspective) const override;
    [[nodiscard]] EvaluationBreakdown breakdown(const GameState&, Color perspective) const;
    [[nodiscard]] const ClassicalEvaluationParameters& parameters() const noexcept {
        return kClassicalEvaluationParameters;
    }
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
};

} // namespace koi
