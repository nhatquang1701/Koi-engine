#pragma once

#include "koi/evaluator.hpp"

namespace koi {

class ClassicalEvaluator final : public Evaluator {
public:
    [[nodiscard]] int evaluate(const GameState&, Color perspective) const override;
};

} // namespace koi
