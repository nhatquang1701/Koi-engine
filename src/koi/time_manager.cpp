#include "koi/time_manager.hpp"

#include <algorithm>

namespace koi {
namespace {

std::chrono::milliseconds with_safety_margin(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }

    const auto margin = std::min(std::chrono::milliseconds{50}, std::max(std::chrono::milliseconds{1}, duration / 20));
    return duration > margin ? duration - margin : std::chrono::milliseconds{1};
}

} // namespace

TimeManager::TimeManager(SearchLimits limits, Color side_to_move)
    : limits_(std::move(limits)), started_(std::chrono::steady_clock::now()) {
    if (limits_.infinite) {
        return;
    }

    if (limits_.movetime.has_value()) {
        budget_ = with_safety_margin(*limits_.movetime);
        return;
    }

    const std::optional<ClockLimit>& clock = side_to_move == Color::white ? limits_.white_clock : limits_.black_clock;
    if (!clock.has_value()) {
        return;
    }

    const std::uint32_t moves = std::max<std::uint32_t>(1, limits_.moves_to_go.value_or(30));
    const auto base = clock->remaining / moves;
    const auto allocation = std::min(clock->remaining, base + (clock->increment * 3) / 4);
    budget_ = with_safety_margin(allocation);
}

std::optional<std::chrono::milliseconds> TimeManager::time_budget() const noexcept {
    return budget_;
}

bool TimeManager::should_stop(std::uint64_t nodes) const noexcept {
    if (limits_.nodes.has_value() && nodes >= *limits_.nodes) {
        return true;
    }
    if (limits_.infinite || !budget_.has_value()) {
        return false;
    }
    return std::chrono::steady_clock::now() - started_ >= *budget_;
}

} // namespace koi
