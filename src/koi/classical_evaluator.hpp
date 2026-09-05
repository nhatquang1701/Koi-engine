#pragma once

#include "koi/evaluator.hpp"
#include "koi/evaluation_parameters.hpp"

namespace koi {

struct EvaluationBreakdown {
    int material = 0;
    int piece_square = 0;
    int mobility = 0;
    int pawn_structure = 0;
    int activity = 0;
    int king_safety = 0;
    int king_activity = 0;
    int passed_pawn = 0;
    int tempo = 0;
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
