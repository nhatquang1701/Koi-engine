#pragma once

#include "koi/game_state.hpp"

namespace koi {

class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color perspective) const = 0;

    // Search may invoke an evaluator concurrently when Threads > 1. The
    // default keeps third-party evaluators safe by having the search service
    // serialize calls unless an implementation explicitly opts in.
    [[nodiscard]] virtual bool supports_concurrent_evaluation() const noexcept {
        return false;
    }
};

} // namespace koi
