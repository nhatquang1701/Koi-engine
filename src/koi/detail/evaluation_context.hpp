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

    // Advisory lifecycle notifications forwarded to the private worker, if any.
    void notify_make_move(const GameState& state, const MoveMetadata& metadata,
                          int ply, std::uint64_t parent_key) {
        if (worker_) {
            worker_->on_make_move(state, metadata, ply, parent_key);
        }
    }
    void notify_unmake_move(int child_ply) {
        if (worker_) {
            worker_->on_unmake_move(child_ply);
        }
    }
    void notify_make_null_move(const GameState& state, int ply,
                               std::uint64_t parent_key) {
        if (worker_) {
            worker_->on_make_null_move(state, ply, parent_key);
        }
    }
    void notify_unmake_null_move(int child_ply) {
        if (worker_) {
            worker_->on_unmake_null_move(child_ply);
        }
    }

private:
    const Evaluator& evaluator_;
    std::mutex* evaluator_mutex_ = nullptr;
    std::unique_ptr<EvaluatorWorker> worker_;
};

} // namespace koi::detail
