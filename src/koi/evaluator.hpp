#pragma once

#include <memory>

#include "koi/game_state.hpp"

namespace koi {

class EvaluatorWorker {
public:
    virtual ~EvaluatorWorker() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color) = 0;
};

class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color perspective) const = 0;

    // A worker is optional. Stateful evaluators can use it to keep accumulator
    // or cache state local to one search context; stateless evaluators retain
    // the existing shared evaluation path.
    [[nodiscard]] virtual std::unique_ptr<EvaluatorWorker> create_worker() const {
        return {};
    }

    // Search may invoke an evaluator concurrently when Threads > 1. The
    // default keeps third-party evaluators safe by having the search service
    // serialize calls unless an implementation explicitly opts in.
    [[nodiscard]] virtual bool supports_concurrent_evaluation() const noexcept {
        return false;
    }
};

} // namespace koi
