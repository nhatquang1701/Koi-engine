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

private:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept;
    [[nodiscard]] static int initial_hardness(const RootTimingContext& context) noexcept;

    SearchLimits limits_;
    std::optional<std::chrono::milliseconds> budget_;
    RootTimingContext root_context_;
    TimePointProvider now_;
    std::chrono::steady_clock::time_point started_;
    TimeManagementStats timing_;
    std::optional<int> previous_score_;
    std::uint32_t stable_observations_ = 0;
    bool hard_position_ = false;
    bool clock_mode_ = false;
    bool emergency_pacing_ = false;
    mutable std::atomic_bool hard_deadline_reached_ = false;
};

} // namespace koi
