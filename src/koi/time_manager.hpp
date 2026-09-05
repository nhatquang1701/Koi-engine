#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include "koi/search_types.hpp"

namespace koi {

class TimeManager {
public:
    TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent = 100,
                std::uint32_t move_overhead_ms = 10,
                std::uint32_t slow_mover_percent = 100);

    [[nodiscard]] std::optional<std::chrono::milliseconds> time_budget() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> node_limit() const noexcept;
    [[nodiscard]] bool should_stop(std::uint64_t nodes) const noexcept;

private:
    SearchLimits limits_;
    std::optional<std::chrono::milliseconds> budget_;
    std::chrono::steady_clock::time_point started_;
};

} // namespace koi
