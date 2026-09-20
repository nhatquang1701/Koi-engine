#include "koi/time_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

namespace koi {
namespace {

std::chrono::milliseconds with_safety_margin(std::chrono::milliseconds duration) noexcept {
    if (duration <= std::chrono::milliseconds::zero()) {
        return std::chrono::milliseconds::zero();
    }

    const auto margin = std::min(std::chrono::milliseconds{50},
                                 std::max(std::chrono::milliseconds{1}, duration / 20));
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

std::chrono::milliseconds scale_fraction(std::chrono::milliseconds duration,
                                         std::uint64_t numerator,
                                         std::uint64_t denominator) noexcept {
    if (duration <= std::chrono::milliseconds::zero() || numerator == 0) {
        return std::chrono::milliseconds::zero();
    }
    using Rep = std::chrono::milliseconds::rep;
    const std::uint64_t count = static_cast<std::uint64_t>(duration.count());
    const std::uint64_t maximum = static_cast<std::uint64_t>(std::numeric_limits<Rep>::max());
    if (count > maximum / numerator) {
        return std::chrono::milliseconds{std::numeric_limits<Rep>::max()};
    }
    return std::chrono::milliseconds{static_cast<Rep>((count * numerator) / denominator)};
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
    return scale_fraction(duration, 3, 4);
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

std::chrono::milliseconds nonnegative_difference(std::chrono::milliseconds left,
                                                 std::chrono::milliseconds right) noexcept {
    return left > right ? left - right : std::chrono::milliseconds::zero();
}

} // namespace

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                         std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent)
    : TimeManager(std::move(limits), side_to_move, speed_percent, move_overhead_ms,
                  slow_mover_percent, {}, {}) {}

TimeManager::TimeManager(SearchLimits limits, Color side_to_move, std::uint8_t speed_percent,
                         std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent,
                         RootTimingContext root_context,
                         TimePointProvider now)
    : limits_(std::move(limits)), root_context_(root_context), now_(std::move(now)), started_(this->now()) {
    initialize(side_to_move, speed_percent, move_overhead_ms, slow_mover_percent);
}

void TimeManager::reconfigure(const SearchLimits& limits, Color side_to_move,
                              std::uint8_t speed_percent, std::uint32_t move_overhead_ms,
                              std::uint32_t slow_mover_percent,
                              const RootTimingContext& root_context) {
    limits_ = limits;
    root_context_ = root_context;
    started_ = now();
    initialize(side_to_move, speed_percent, move_overhead_ms, slow_mover_percent);
}

void TimeManager::initialize(Color side_to_move, std::uint8_t speed_percent,
                             std::uint32_t move_overhead_ms, std::uint32_t slow_mover_percent) {
    budget_.reset();
    timing_ = TimeManagementStats{};
    previous_score_.reset();
    stable_observations_ = 0;
    hard_position_ = false;
    clock_mode_ = false;
    emergency_pacing_ = false;
    hard_deadline_reached_.store(false, std::memory_order_relaxed);

    timing_.initial_hardness = initial_hardness(root_context_);
    timing_.observed_hardness = timing_.initial_hardness;
    hard_position_ = timing_.initial_hardness >= 35;
    timing_.extended_for_hard_position = hard_position_;
    timing_.horizon = std::max<std::uint32_t>(1, limits_.moves_to_go.value_or(20));

    // Explicit depth and node limits are strict upper bounds, but they must not
    // disable the clock guard: a GUI may send `go depth N wtime ...` (or mix a
    // node budget with clocks) and still expect the engine to answer inside the
    // remaining clock. Only an unbounded search has no deadline. A bare
    // `go depth N` / `go nodes N` remains untimed because it carries neither a
    // movetime nor a clock and returns at the clock check below.
    if (limits_.infinite || limits_.ponder) {
        return;
    }

    if (limits_.movetime.has_value()) {
        const auto slow = scale_duration(*limits_.movetime, slow_mover_percent);
        const auto scaled = scale_duration(slow, speed_percent);
        budget_ = with_safety_margin(subtract_overhead(scaled, move_overhead_ms));
        timing_.usable = *budget_;
        timing_.soft_budget = *budget_;
        timing_.hard_budget = *budget_;
        return;
    }

    const std::optional<ClockLimit>& clock = side_to_move == Color::white ?
        limits_.white_clock : limits_.black_clock;
    if (!clock.has_value()) {
        return;
    }

    clock_mode_ = true;
    const auto remaining = std::max(clock->remaining, std::chrono::milliseconds::zero());
    const auto overhead = std::chrono::milliseconds{
        static_cast<std::chrono::milliseconds::rep>(std::min<std::uint64_t>(
            move_overhead_ms, static_cast<std::uint64_t>(
                std::numeric_limits<std::chrono::milliseconds::rep>::max())))};
    const auto reserve_floor = std::max(
        std::chrono::milliseconds{50},
        std::max(scale_duration(remaining, 3), saturating_add(overhead, overhead)));
    timing_.reserve = std::min(std::chrono::milliseconds{750}, reserve_floor);
    timing_.reserve = std::min(timing_.reserve, remaining);
    timing_.usable = nonnegative_difference(remaining, timing_.reserve);
    emergency_pacing_ = remaining <= std::chrono::milliseconds{3000};
    timing_.emergency_pacing = emergency_pacing_;

    const auto base_allocation = timing_.usable / timing_.horizon;
    // The increment may only pay for a bounded share of the clock. On a fast
    // control such as 6+1 an uncapped 3/4 increment credit alone can approach
    // the whole remaining time and leave a single move free to burn it.
    const auto increment_credit = std::min(three_quarters(clock->increment),
                                           scale_fraction(timing_.usable, 1, 4));
    const auto raw_soft = saturating_add(base_allocation, increment_credit);
    const auto raw_hard = saturating_add(scale_fraction(base_allocation, 3, 1),
                                         scale_fraction(increment_credit, 3, 2));
    const auto scale_budget = [slow_mover_percent, speed_percent, this](
                                  std::chrono::milliseconds duration) {
        const auto slow = scale_duration(duration, slow_mover_percent);
        return std::min(timing_.usable, scale_duration(slow, speed_percent));
    };
    timing_.soft_budget = scale_budget(std::min(raw_soft, timing_.usable));
    timing_.hard_budget = scale_budget(std::min(raw_hard, timing_.usable));
    timing_.soft_budget = std::min(timing_.soft_budget, timing_.hard_budget);

    // A three-move extension is useful with a normal clock, but it is unsafe
    // when the entire remaining clock is only a few seconds. TT misses are
    // common in that regime and would otherwise classify almost every move as
    // hard, allowing repeated hard budgets to consume the whole clock. Pace
    // low-clock searches over three times the normal horizon and retain only
    // a small hard-position extension while preserving the reserve.
    if (emergency_pacing_) {
        const std::uint32_t emergency_horizon = timing_.horizon <=
                std::numeric_limits<std::uint32_t>::max() / 3 ?
            timing_.horizon * 3 : std::numeric_limits<std::uint32_t>::max();
        const auto emergency_base = timing_.usable / std::max<std::uint32_t>(1, emergency_horizon);
        const auto emergency_soft = std::min(
            timing_.usable, saturating_add(emergency_base, increment_credit));
        const auto emergency_extension = std::max(
            std::chrono::milliseconds{5}, emergency_base / 2);
        const auto emergency_hard = std::min(
            timing_.usable, saturating_add(emergency_soft, emergency_extension));
        timing_.soft_budget = std::min(timing_.soft_budget, emergency_soft);
        if (!hard_position_) {
            timing_.hard_budget = std::min(timing_.hard_budget, emergency_hard);
        }
        timing_.hard_budget = std::max(timing_.hard_budget, timing_.soft_budget);
    }

    // Flag-proof ceiling. Every heuristic above (increment credit, position
    // hardness, a skipped emergency reduction) can raise the hard budget up to
    // the whole usable clock, which would leave only the reserve between the
    // engine and a flag once the teardown and the GUI round trip are paid for.
    // These caps are applied last so nothing can bypass them: a single move may
    // only ever spend a quarter of the usable clock, and it must always leave a
    // safety margin that covers the move overhead and search teardown.
    const auto hard_safety = std::max(
        saturating_add(overhead, saturating_add(overhead, std::chrono::milliseconds{15})),
        std::min(std::chrono::milliseconds{50}, remaining / 20));
    const auto usable_ceiling = nonnegative_difference(timing_.usable, hard_safety);
    const auto fraction_ceiling = std::max(std::chrono::milliseconds{5},
                                           scale_fraction(timing_.usable, 1, 4));
    const auto capped_hard = std::min(std::min(timing_.hard_budget, fraction_ceiling),
                                      usable_ceiling);
    // A one-millisecond floor keeps the deadline comparable and still fires on
    // the first stop check rather than never.
    timing_.hard_budget = std::max(std::chrono::milliseconds{1}, capped_hard);
    timing_.soft_budget = std::min(timing_.soft_budget, timing_.hard_budget);
    budget_ = timing_.hard_budget;
}

std::chrono::steady_clock::time_point TimeManager::now() const noexcept {
    if (!now_) {
        return std::chrono::steady_clock::now();
    }
    try {
        return now_();
    } catch (...) {
        return std::chrono::steady_clock::now();
    }
}

int TimeManager::initial_hardness(const RootTimingContext& context) noexcept {
    int hardness = 0;
    if (!context.table_available || !context.tt_hit) {
        hardness += 30;
    }
    if (!context.tt_exact) {
        hardness += 15;
    }
    if (context.tt_depth < 6) {
        hardness += 15;
    }
    if (context.tt_generation_age > 2) {
        hardness += 10;
    }
    if (!context.tt_has_best_move) {
        hardness += 10;
    }
    if (context.in_check) {
        hardness += 15;
    }
    if (context.forcing_move_count > 0 &&
        context.forcing_move_count * 2 >= context.legal_move_count) {
        hardness += 10;
    }
    if (context.legal_move_count >= 30) {
        hardness += 5;
    }
    if (context.tt_hit && context.tt_exact && context.tt_depth >= 8 &&
        context.tt_generation_age <= 1) {
        hardness -= 25;
    }
    return std::clamp(hardness, 0, 100);
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
    if (now() - started_ >= *budget_) {
        hard_deadline_reached_.store(true, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void TimeManager::observe_iteration(const SearchIterationObservation& observation) noexcept {
    bool score_swing = false;
    if (previous_score_.has_value()) {
        const long long delta = static_cast<long long>(observation.score_cp) -
            static_cast<long long>(*previous_score_);
        score_swing = std::llabs(delta) >= 75;
    }
    const bool hard_evidence = observation.best_move_changed || observation.pv_changed ||
        score_swing || observation.aspiration_researched;
    if (hard_evidence) {
        hard_position_ = true;
        // The stop deadline is fixed when the search starts and the safety
        // ceilings were applied there. Hard evidence must therefore not raise
        // the hard budget here; it only feeds diagnostics and iteration pacing.
        stable_observations_ = 0;
        timing_.extended_for_hard_position = true;
        timing_.observed_hardness = std::max(timing_.observed_hardness, 100);
    } else if (previous_score_.has_value()) {
        const long long delta = static_cast<long long>(observation.score_cp) -
            static_cast<long long>(*previous_score_);
        if (!observation.best_move_changed && !observation.pv_changed &&
            std::llabs(delta) <= 25 && !observation.aspiration_researched) {
            ++stable_observations_;
        } else {
            stable_observations_ = 0;
        }
    } else {
        stable_observations_ = 1;
    }
    previous_score_ = observation.score_cp;
}

bool TimeManager::should_stop_after_iteration() const noexcept {
    if (limits_.infinite || limits_.ponder || !budget_.has_value()) {
        return false;
    }
    const auto elapsed = now() - started_;
    if (elapsed >= *budget_) {
        hard_deadline_reached_.store(true, std::memory_order_relaxed);
        return true;
    }
    if (emergency_pacing_ && !hard_position_ && elapsed >= timing_.soft_budget) {
        return true;
    }
    return clock_mode_ && !hard_position_ && stable_observations_ >= 2 &&
        elapsed >= timing_.soft_budget;
}

bool TimeManager::should_start_next_iteration(std::chrono::milliseconds estimated_next_iteration) const noexcept {
    if (limits_.infinite || limits_.ponder || !budget_.has_value()) {
        return true;
    }
    const auto elapsed = now() - started_;
    if (emergency_pacing_ && !hard_position_ && elapsed >= timing_.soft_budget) {
        return false;
    }
    return elapsed < *budget_ && elapsed +
        std::max(estimated_next_iteration, std::chrono::milliseconds{1}) < *budget_;
}

TimeManagementStats TimeManager::diagnostics() const noexcept {
    TimeManagementStats result = timing_;
    result.hard_deadline_reached = hard_deadline_reached_.load(std::memory_order_relaxed);
    return result;
}

} // namespace koi
