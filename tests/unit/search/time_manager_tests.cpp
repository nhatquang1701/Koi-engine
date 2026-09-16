#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/time_manager.hpp"

namespace {

using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;
using koi::ClockLimit;
using koi::RootTimingContext;
using koi::SearchIterationObservation;
using koi::SearchLimits;
using koi::TimeManagementStats;
using koi::TimeManager;

constexpr std::uint32_t kMoveOverheadMs = 30;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

SearchLimits clock_limits(milliseconds remaining, milliseconds increment) {
    SearchLimits limits;
    limits.white_clock = ClockLimit{remaining, increment};
    limits.black_clock = ClockLimit{remaining, increment};
    return limits;
}

// A warm TT yields a low initial hardness (letting the emergency reduction
// apply); the default context is cold and therefore always a "hard" position.
RootTimingContext warm_context() {
    RootTimingContext context;
    context.table_available = true;
    context.tt_hit = true;
    context.tt_exact = true;
    context.tt_has_best_move = true;
    context.tt_depth = 10;
    context.tt_generation_age = 0;
    return context;
}

milliseconds expected_hard_safety(milliseconds remaining) {
    const milliseconds overhead{kMoveOverheadMs};
    const auto doubled_overhead = overhead + overhead + milliseconds{15};
    const auto fraction = std::min(milliseconds{50}, milliseconds{remaining.count() / 20});
    return std::max(doubled_overhead, fraction);
}

milliseconds expected_fraction_ceiling(milliseconds usable) {
    return std::max(milliseconds{5}, milliseconds{usable.count() / 4});
}

milliseconds nonnegative_difference(milliseconds value, milliseconds amount) {
    return value > amount ? value - amount : milliseconds{0};
}

TimeManager make_clock_manager(milliseconds remaining, milliseconds increment,
                               std::optional<std::uint32_t> moves_to_go, bool warm) {
    SearchLimits limits = clock_limits(remaining, increment);
    limits.moves_to_go = moves_to_go;
    return TimeManager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       warm ? warm_context() : RootTimingContext{});
}

// The headline guarantee: whatever the clock, the increment, the horizon or the
// detected position hardness, the enforced deadline leaves the safety margin and
// never spends more than a quarter of the usable clock on a single move.
void test_clock_budget_never_breaks_the_safety_invariants() {
    const milliseconds overage{kMoveOverheadMs};
    (void)overage;

    const milliseconds remainings[] = {milliseconds{600},  milliseconds{1500}, milliseconds{2500},
                                       milliseconds{6000}, milliseconds{15000}, milliseconds{60000},
                                       milliseconds{600000}};
    const milliseconds increments[] = {milliseconds{0}, milliseconds{1000}, milliseconds{5000},
                                       milliseconds{35000}};
    const std::optional<std::uint32_t> horizons[] = {std::nullopt, 20, 40};
    const bool hardness[] = {false, true};

    for (const milliseconds remaining : remainings) {
        for (const milliseconds increment : increments) {
            for (const std::optional<std::uint32_t> moves_to_go : horizons) {
                for (const bool warm : hardness) {
                    TimeManager manager =
                        make_clock_manager(remaining, increment, moves_to_go, warm);
                    const auto budget = manager.time_budget();
                    require(budget.has_value(), "a clock search must always allocate a budget");

                    const TimeManagementStats stats = manager.diagnostics();
                    const auto safety = expected_hard_safety(remaining);
                    const auto ceiling = std::max(milliseconds{1}, nonnegative_difference(stats.usable, safety));

                    require(stats.usable <= remaining, "usable time must not exceed the clock");
                    require(*budget <= ceiling,
                            "the enforced deadline must leave the safety margin on the clock");
                    require(*budget <= expected_fraction_ceiling(stats.usable),
                            "a single move must not exceed a quarter of the usable clock");
                    require(*budget < remaining, "the deadline must be smaller than the clock");
                    require(*budget >= milliseconds{1}, "the deadline must never be zero");
                    require(stats.hard_budget == *budget,
                            "diagnostics must report the enforced deadline");
                    require(stats.soft_budget <= stats.hard_budget,
                            "the soft budget must not exceed the hard budget");
                    require(stats.reserve <= remaining, "the reserve must not exceed the clock");
                    if (stats.usable > milliseconds{0}) {
                        require(*budget <= stats.usable,
                                "the deadline must fit inside the usable clock");
                    }
                    if (remaining >= milliseconds{2000}) {
                        require(stats.reserve.count() >= 2 * kMoveOverheadMs,
                                "the reserve must cover at least two move overheads");
                    }
                }
            }
        }
    }
}

// Regression guard for the 6+1 endgame that lost on time: with 1.5 s left and a
// 1 s increment the old arithmetic allocated ~1.45 s in a single move.
void test_fast_control_move_leaves_most_of_the_clock() {
    TimeManager manager = make_clock_manager(milliseconds{1500}, milliseconds{1000},
                                             std::nullopt, false);
    const auto budget = manager.time_budget();
    require(budget.has_value(), "a clock search must allocate a budget");
    require(*budget <= milliseconds{400},
            "a 6+1 move must not consume the whole remaining clock");
    require(*budget * 3 <= milliseconds{1500},
            "at least two thirds of the clock must survive a single move");
}

void test_low_clock_engages_emergency_pacing() {
    TimeManager emergency = make_clock_manager(milliseconds{2000}, milliseconds{100},
                                               std::nullopt, true);
    require(emergency.diagnostics().emergency_pacing,
            "a clock of two seconds must engage emergency pacing");

    TimeManager normal = make_clock_manager(milliseconds{6000}, milliseconds{100},
                                            std::nullopt, true);
    require(!normal.diagnostics().emergency_pacing,
            "a healthy clock must not engage emergency pacing");
}

void test_movetime_keeps_its_own_budget() {
    SearchLimits limits;
    limits.movetime = milliseconds{1000};
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100);
    const auto budget = manager.time_budget();
    require(budget.has_value(), "a movetime search must allocate a budget");
    require(*budget <= milliseconds{1000} - milliseconds{kMoveOverheadMs},
            "a movetime budget must pay the move overhead");
    require(*budget >= milliseconds{800},
            "a one second movetime budget must stay close to one second");
    require(manager.diagnostics().hard_budget == *budget,
            "movetime diagnostics must report the enforced deadline");
}

void test_deadline_is_fixed_at_construction_and_cannot_be_extended() {
    auto current = std::make_shared<Clock::time_point>(Clock::now());
    SearchLimits limits = clock_limits(milliseconds{60000}, milliseconds{0});
    TimeManager manager(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                        RootTimingContext{}, [current]() { return *current; });

    const auto budget = manager.time_budget();
    require(budget.has_value(), "a clock search must allocate a budget");
    const auto allocated = *budget;

    require(!manager.should_stop(0), "the search must not stop before the deadline");
    *current += allocated - milliseconds{1};
    require(!manager.should_stop(0), "the search must not stop before the allocated time");
    *current += milliseconds{1};
    require(manager.should_stop(0), "the search must stop once the allocated time is spent");
    require(manager.diagnostics().hard_deadline_reached,
            "the deadline flag must be visible in diagnostics");

    // Hard evidence drives diagnostics and iteration pacing only; it must never
    // buy additional wall-clock time beyond the frozen deadline.
    auto second_clock = std::make_shared<Clock::time_point>(Clock::now());
    TimeManager second(limits, koi::Color::white, 100, kMoveOverheadMs, 100,
                       RootTimingContext{}, [second_clock]() { return *second_clock; });
    const auto second_budget = second.time_budget();
    require(second_budget.has_value(), "a clock search must allocate a budget");

    SearchIterationObservation observation;
    observation.depth = 12;
    observation.score_cp = 400;
    observation.best_move_changed = true;
    observation.pv_changed = true;
    observation.aspiration_researched = true;
    second.observe_iteration(observation);

    require(second.time_budget() == second_budget,
            "an iteration observation must not move the enforced deadline");
    require(second.diagnostics().hard_budget == *second_budget,
            "diagnostics must keep reporting the enforced deadline");
    require(second.diagnostics().observed_hardness >= 100,
            "hard evidence must still be recorded for diagnostics");
    require(second.diagnostics().extended_for_hard_position,
            "hard evidence must be recorded as an extended hard position");

    *second_clock += *second_budget - milliseconds{20};
    require(second.should_start_next_iteration(milliseconds{1}),
            "an iteration that still fits must be allowed to start");
    require(!second.should_start_next_iteration(milliseconds{50}),
            "an iteration that cannot finish must not start");
    *second_clock += milliseconds{20};
    require(second.should_stop_after_iteration(),
            "the search must stop after the iteration that reaches the deadline");
}

// Worst case simulation: every move spends the entire enforced deadline and only
// the increment comes back. The safety margin must hold on every single move and
// an incremental control must retain a healthy clock. A control without an
// increment necessarily drains towards the reserve over a long game - that is
// physics, not a flag - but the engine must still never allocate more than it
// has and must never reach zero.
void test_synthetic_games_never_reach_the_flag() {
    struct Control {
        const char* name;
        std::int64_t remaining_ms;
        std::int64_t increment_ms;
    };
    const Control controls[] = {
        {"6+1", 6000, 1000},  {"15+0", 15000, 0}, {"3+2", 3000, 2000},
        {"1+0", 1000, 0},     {"60+0", 60000, 0}, {"2.5+1", 2500, 1000},
    };

    for (const Control& control : controls) {
        milliseconds remaining{control.remaining_ms};
        const milliseconds increment{control.increment_ms};
        for (int move = 0; move < 80; ++move) {
            TimeManager manager = make_clock_manager(remaining, increment, std::nullopt, false);
            const auto budget = manager.time_budget();
            require(budget.has_value(), "a simulated move must allocate a budget");

            const TimeManagementStats stats = manager.diagnostics();
            const auto safety = expected_hard_safety(remaining);
            const auto ceiling =
                std::max(milliseconds{1}, nonnegative_difference(stats.usable, safety));

            require(*budget <= ceiling,
                    "a simulated move must leave the safety margin on the clock");
            require(*budget >= milliseconds{1}, "a simulated move must allocate some time");
            if (remaining > milliseconds{2}) {
                require(*budget < remaining,
                        "a simulated move must never spend the whole clock");
            }
            remaining = remaining - *budget + increment;
        }
        require(remaining > milliseconds{0}, "the simulated clock must never reach zero");
        if (increment >= milliseconds{500}) {
            require(remaining >= milliseconds{100},
                    "an incremental control must retain a playable clock");
        }
    }
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"clock budget safety invariants", test_clock_budget_never_breaks_the_safety_invariants},
        {"fast control keeps the clock", test_fast_control_move_leaves_most_of_the_clock},
        {"emergency pacing", test_low_clock_engages_emergency_pacing},
        {"movetime budget", test_movetime_keeps_its_own_budget},
        {"frozen deadline", test_deadline_is_fixed_at_construction_and_cannot_be_extended},
        {"synthetic games never flag", test_synthetic_games_never_reach_the_flag},
    };
    int failures = 0;
    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
