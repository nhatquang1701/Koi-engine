#pragma once

#include <memory>
#include <mutex>

#include "koi/evaluator.hpp"

namespace koi::detail {

// Owns evaluation execution state for one search context. Stateful evaluator
// workers stay local; stateless evaluators retain the existing guarded path.
class EvaluationContext final {
public:
    EvaluationContext(const Evaluator& evaluator, std::mutex* evaluator_mutex);
    EvaluationContext(const EvaluationContext&) = delete;
    EvaluationContext& operator=(const EvaluationContext&) = delete;
    EvaluationContext(EvaluationContext&&) = delete;
    EvaluationContext& operator=(EvaluationContext&&) = delete;
    ~EvaluationContext();

    [[nodiscard]] int evaluate(const GameState&, Color perspective);
    [[nodiscard]] bool has_private_worker() const noexcept {
        return static_cast<bool>(worker_);
    }

private:
    const Evaluator& evaluator_;
    std::mutex* evaluator_mutex_ = nullptr;
    std::unique_ptr<EvaluatorWorker> worker_;
};

} // namespace koi::detail
