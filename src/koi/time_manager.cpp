#include "koi/time_manager.hpp"

#include <algorithm>
#include <limits>

namespace koi {
namespace {

std::chrono::milliseconds with_safety_margin(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }

    const auto margin = std::min(std::chrono::milliseconds{50}, std::max(std::chrono::milliseconds{1}, duration / 20));
    return duration > margin ? duration - margin : std::chrono::milliseconds{1};
}

std::chrono::milliseconds scale_duration(std::chrono::milliseconds duration,
                                         std::uint8_t speed_percent) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return duration;
    }

    const std::uint64_t speed = std::clamp<std::uint64_t>(speed_percent, 1, 100);
    using Rep = std::chrono::milliseconds::rep;
    const std::uint64_t count = static_cast<std::uint64_t>(duration.count());
    // Split before multiplying so even milliseconds::max() scales exactly
    // without requiring a compiler-specific wider integer type.
    const std::uint64_t scaled = (count / 100) * speed + ((count % 100) * speed) / 100;
    return std::chrono::milliseconds{static_cast<Rep>(scaled)};
}

std::chrono::milliseconds three_quarters(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }

    using Rep = std::chrono::milliseconds::rep;
    const Rep count = duration.count();
    // Compute floor(3 * count / 4) without multiplying count itself.
    const Rep quotient = count / Rep{4};
    const Rep remainder = count % Rep{4};
    return std::chrono::milliseconds{quotient * Rep{3} + (remainder * Rep{3}) / Rep{4}};
}

std::chrono::milliseconds saturating_add(std::chrono::milliseconds left,
                                         std::chrono::milliseconds right) noexcept {
    using Rep = std::chrono::milliseconds::rep;
    const Rep left_count = left.count();
    const Rep right_count = right.count();
    const Rep maximum = std::numeric_limits<Rep>::max();
    const Rep minimum = std::numeric_limits<Rep>::lowest();

    if (right_count > 0 && left_count > maximum - right_count) {
        return std::chrono::milliseconds{maximum};
    }
    if (right_count < 0 && left_count < minimum - right_count) {
        return std::chrono::milliseconds{minimum};
    }
    return std::chrono::milliseconds{left_count + right_count};
}

} // namespace

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent)
    : limits_(std::move(limits)), started_(std::chrono::steady_clock::now()) {
    if (limits_.infinite || limits_.ponder) {
        return;
    }

    if (limits_.movetime.has_value()) {
        budget_ = with_safety_margin(scale_duration(*limits_.movetime, speed_percent));
        return;
    }

    const std::optional<ClockLimit>& clock = side_to_move == Color::white ? limits_.white_clock : limits_.black_clock;
    if (!clock.has_value()) {
        return;
    }

    const std::uint32_t moves = std::max<std::uint32_t>(1, limits_.moves_to_go.value_or(30));
    const auto base = clock->remaining / moves;
    const auto allocation = std::min(clock->remaining, saturating_add(base, three_quarters(clock->increment)));
    budget_ = with_safety_margin(scale_duration(allocation, speed_percent));
}

std::optional<std::chrono::milliseconds> TimeManager::time_budget() const noexcept {
    return budget_;
}

std::optional<std::uint64_t> TimeManager::node_limit() const noexcept {
    return limits_.nodes;
}

bool TimeManager::should_stop(std::uint64_t nodes) const noexcept {
    if (const std::optional<std::uint64_t> limit = node_limit(); limit.has_value() &&
        nodes >= *limit) {
        return true;
    }
    if (limits_.infinite || limits_.ponder || !budget_.has_value()) {
        return false;
    }
    return std::chrono::steady_clock::now() - started_ >= *budget_;
}

} // namespace koi
