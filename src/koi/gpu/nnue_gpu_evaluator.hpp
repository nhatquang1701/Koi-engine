#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "koi/evaluator.hpp"

namespace koi {
class GameState;

namespace gpu {
class GpuNnueService;

// UCI-selectable GPU inference policy (`GpuNnue`, default `auto`).
// Precedence, most restrictive first: an explicit `off` from either the option
// or `KOI_GPU_NNUE=0` always disables the GPU; otherwise a non-empty
// `KOI_GPU_NNUE` other than "0" forces `require` over the option; otherwise the
// option decides.  The environment variables stay test overrides, so they can
// request the GPU even when the option says `auto`, but they can never turn a
// user's explicit `off` back on.
enum class GpuNnueMode : std::uint8_t {
    automatic,
    require,
    disabled,
};

[[nodiscard]] std::string_view gpu_nnue_mode_name(GpuNnueMode mode) noexcept;
[[nodiscard]] bool parse_gpu_nnue_mode(std::string_view text, GpuNnueMode& mode) noexcept;

// The option's current value.
[[nodiscard]] GpuNnueMode gpu_nnue_mode() noexcept;
void set_gpu_nnue_mode(GpuNnueMode mode) noexcept;

// `KOI_GPU_NNUE` read once per process; nullopt when it does not override.
[[nodiscard]] std::optional<GpuNnueMode> gpu_nnue_env_override() noexcept;
// Test seam: the environment is cached on first use, so a test that changes
// `KOI_GPU_NNUE` in-process must clear the cache afterwards.
void reset_gpu_nnue_environment_cache() noexcept;

// The mode actually applied: the environment override combined with the option.
[[nodiscard]] GpuNnueMode effective_gpu_nnue_mode() noexcept;

// Build-level availability: the embedded kernel exists and the driver loads.
// The driver probe result is cached for the process lifetime.
[[nodiscard]] bool gpu_nnue_available() noexcept;

// Outcome of the most recent evaluator installation, recorded by
// `maybe_wrap_gpu_nnue` so the UCI layer can explain one line per search.
struct GpuNnueStatus {
    bool network_loaded = false;
    bool service_ready = false;
    std::string device_name;
    std::string reason;
};

void record_gpu_nnue_status(bool network_loaded, bool service_ready,
                            std::string device_name, std::string reason) noexcept;
[[nodiscard]] GpuNnueStatus gpu_nnue_status();

// One `info string` payload describing the GPU decision for a search with
// `threads` search threads.  Empty when no NNUE network is active, so the
// controller emits nothing for the classical evaluator.
[[nodiscard]] std::string gpu_nnue_search_diagnostic(std::size_t threads);

// Search-wide gate: the GPU path only serves multi-threaded searches so the
// deterministic single-threaded path stays on the CPU implementation.  The gate
// is a counter rather than a flag so overlapping searches with different thread
// counts cannot flip each other's evaluator path mid-search.
void begin_gpu_nnue_threaded_search() noexcept;
void end_gpu_nnue_threaded_search() noexcept;
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
