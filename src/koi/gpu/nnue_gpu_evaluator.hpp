#pragma once

#include <memory>

#include "koi/evaluator.hpp"

namespace koi {
class GameState;

namespace gpu {
class GpuNnueService;

// Build-level availability: the embedded kernel exists and the driver loaded.
[[nodiscard]] bool gpu_nnue_available() noexcept;

// Search-wide gate: the GPU path only serves multi-threaded searches so the
// deterministic single-threaded path stays on the CPU implementation.
void set_gpu_nnue_threaded(bool threaded) noexcept;
[[nodiscard]] bool gpu_nnue_threaded() noexcept;

// Evaluator that dispatches single positions to the GPU batch service and
// falls back to the wrapped CPU evaluator (and its private worker) whenever the
// service is unavailable, disabled, or fails mid-search.
class GpuNnueEvaluator final : public Evaluator {
public:
    GpuNnueEvaluator(std::shared_ptr<const Evaluator> fallback,
                     std::shared_ptr<GpuNnueService> service);

    [[nodiscard]] int evaluate(const GameState& state, Color perspective) const override;
    [[nodiscard]] std::unique_ptr<EvaluatorWorker> create_worker() const override;
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    [[nodiscard]] bool gpu_enabled() const noexcept;
    // Returns false when the GPU path is skipped so the caller can fall back.
    [[nodiscard]] bool evaluate_on_gpu(const GameState& state, Color perspective,
                                       int& score) const;

private:
    std::shared_ptr<const Evaluator> fallback_;
    std::shared_ptr<GpuNnueService> service_;
};

} // namespace gpu
} // namespace koi
