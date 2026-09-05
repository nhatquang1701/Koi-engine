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
                                         std::uint32_t percent,
                                         std::uint32_t denominator = 100) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return duration;
    }

    const std::uint64_t scale = std::max<std::uint32_t>(1, percent);
    using Rep = std::chrono::milliseconds::rep;
    const std::uint64_t count = static_cast<std::uint64_t>(duration.count());
    const std::uint64_t quotient = count / denominator;
    const std::uint64_t remainder = count % denominator;
    const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
    if (quotient > maximum / scale) {
        return std::chrono::milliseconds{std::numeric_limits<Rep>::max()};
    }
    const std::uint64_t whole = quotient * scale;
    const std::uint64_t fractional = (remainder * scale) / denominator;
    const std::uint64_t scaled = whole > maximum - fractional ? maximum : whole + fractional;
    return std::chrono::milliseconds{static_cast<Rep>(scaled)};
}

std::chrono::milliseconds subtract_overhead(std::chrono::milliseconds duration,
                                             std::uint32_t overhead_ms) noexcept {
    using Rep = std::chrono::milliseconds::rep;
    const Rep overhead = static_cast<Rep>(std::min<std::uint64_t>(
        overhead_ms, static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())));
    if (duration.count() <= overhead) {
        return std::chrono::milliseconds{1};
    }
    return duration - std::chrono::milliseconds{overhead};
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

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                         std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent)
    : limits_(std::move(limits)), started_(std::chrono::steady_clock::now()) {
    if (limits_.infinite || limits_.ponder || limits_.depth.has_value() || limits_.nodes.has_value()) {
        return;
    }

    if (limits_.movetime.has_value()) {
        const auto slow = scale_duration(*limits_.movetime, slow_mover_percent);
        const auto scaled = scale_duration(slow, speed_percent);
        budget_ = with_safety_margin(subtract_overhead(scaled, move_overhead_ms));
        return;
    }

    const std::optional<ClockLimit>& clock = side_to_move == Color::white ? limits_.white_clock : limits_.black_clock;
    if (!clock.has_value()) {
        return;
    }

    const std::uint32_t moves = std::max<std::uint32_t>(1, limits_.moves_to_go.value_or(30));
    const auto base = clock->remaining / moves;
    const auto allocation = std::min(clock->remaining, saturating_add(base, three_quarters(clock->increment)));
    const auto slow = scale_duration(allocation, slow_mover_percent);
    const auto scaled = scale_duration(slow, speed_percent);
    budget_ = with_safety_margin(subtract_overhead(scaled, move_overhead_ms));
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
