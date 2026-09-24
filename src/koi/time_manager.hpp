#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "koi/search_types.hpp"

namespace koi {

using TimePointProvider = std::function<std::chrono::steady_clock::time_point()>;

class TimeManager {
public:
    TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent = 100,
                std::uint32_t move_overhead_ms = 30,
                std::uint32_t slow_mover_percent = 100);
    TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                RootTimingContext root_context, TimePointProvider now = {});

    [[nodiscard]] std::optional<std::chrono::milliseconds> time_budget() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> node_limit() const noexcept;
    [[nodiscard]] bool should_stop(std::uint64_t nodes) const noexcept;
    void observe_iteration(const SearchIterationObservation& observation) noexcept;
    [[nodiscard]] bool should_stop_after_iteration() const noexcept;
    [[nodiscard]] bool should_start_next_iteration(
        std::chrono::milliseconds estimated_next_iteration) const noexcept;
    [[nodiscard]] TimeManagementStats diagnostics() const noexcept;

    // Re-arms the manager for a search that keeps running but changes limits
    // mid-flight (UCI ponderhit). The elapsed clock restarts at the moment of
    // the conversion so the new budget is fully available.
    void reconfigure(const SearchLimits& limits, Color side_to_move, std::uint8_t speed_percent,
                     std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                     const RootTimingContext& root_context);

private:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept;
    [[nodiscard]] std::int64_t now_nanoseconds() const noexcept;
    [[nodiscard]] static int initial_hardness(const RootTimingContext& context) noexcept;
    void initialize(Color side_to_move, std::uint8_t speed_percent, std::uint32_t move_overhead_ms,
                    std::uint32_t slow_mover_percent);

    SearchLimits limits_;
    RootTimingContext root_context_;
    TimePointProvider now_;
    TimeManagementStats timing_;
    std::optional<int> previous_score_;
    std::uint32_t stable_observations_ = 0;
    bool hard_position_ = false;
    bool clock_mode_ = false;
    bool emergency_pacing_ = false;
    mutable std::atomic_bool hard_deadline_reached_ = false;

    // The stop checks run per node (SearchContext::interrupted) and from Lazy
    // SMP helper threads while the main thread may reconfigure the manager on a
    // ponderhit, so every value they read is a plain atomic. `limits_`,
    // `timing_`, and the pacing members above stay main-thread-only.
    std::atomic<std::uint64_t> node_limit_{0};
    std::atomic_bool has_node_limit_{false};
    // Budget in milliseconds; negative means "no deadline". Milliseconds keep
    // very large requests (movetime or clocks near the int64 limit) exactly
    // representable, which a nanosecond budget would overflow.
    std::atomic<std::int64_t> budget_ms_{-1};
    std::atomic<std::int64_t> started_ns_{0};
    std::atomic_bool unbounded_{false};
};

} // namespace koi
