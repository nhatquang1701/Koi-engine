#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/search_ordering_tables.hpp"
#include "koi/detail/search_policy.hpp"

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::Move require_move(const std::string_view uci) {
    const auto parsed = koi::Move::parse_uci(uci);
    require(parsed.has_value(), "test move must parse");
    return *parsed;
}

void test_null_move_policy_owns_window_and_reduction_rules() {
    const auto depth_five = koi::detail::SearchPolicy::null_move(5, 20, 21, false, true);
    require(depth_five.eligible && depth_five.reduction == 2,
            "depth-five null move must use the two-ply reduction");

    const auto depth_six = koi::detail::SearchPolicy::null_move(6, 20, 21, false, true);
    require(depth_six.eligible && depth_six.reduction == 3,
            "deep null move must use the three-ply reduction");

    require(!koi::detail::SearchPolicy::null_move(2, 20, 21, false, true).eligible,
            "shallow nodes must reject null move");
    require(!koi::detail::SearchPolicy::null_move(5, 20, 22, false, true).eligible,
            "wide windows must reject null move");
    require(!koi::detail::SearchPolicy::null_move(5, 20, 21, true, true).eligible,
            "checked nodes must reject null move");
    require(!koi::detail::SearchPolicy::null_move(5, 20, 21, false, false).eligible,
            "disabled null pruning must reject null move");
}

void test_check_extension_policy_owns_timing_and_budget_rules() {
    require(koi::detail::SearchPolicy::check_extension(true, 4, false, 1),
            "checked nodes with budget must extend");
    require(!koi::detail::SearchPolicy::check_extension(false, 4, false, 1),
            "quiet nodes must not receive check extensions");
    require(!koi::detail::SearchPolicy::check_extension(true, 4, true, 1),
            "short timed roots must suppress check extensions");
    require(!koi::detail::SearchPolicy::check_extension(true, 4, false, 0),
            "exhausted check-extension budget must stop extensions");
}

void test_late_move_policy_owns_gating_history_and_reduction() {
    const auto ordinary = koi::detail::SearchPolicy::late_move(
        8, 4, 7, 0, false, false, false, false, false, false, false, true, false);
    require(ordinary.candidate && !ordinary.high_history_exclusion && ordinary.reduced,
            "an ordinary late quiet move must be reduced");
    require(ordinary.reduction == 1, "the baseline late move reduction must be one ply");

    const auto deep = koi::detail::SearchPolicy::late_move(
        12, 20, 11, 0, false, false, false, false, false, false, false, true, false);
    require(deep.candidate && deep.reduction == 3 && deep.reduced,
            "deep late moves must receive the configured depth/move reduction");

    const auto high_history = koi::detail::SearchPolicy::late_move(
        8, 4, 7, 128, false, false, false, false, false, false, false, true, false);
    require(high_history.candidate && high_history.high_history_exclusion &&
                !high_history.reduced,
            "high-history moves must remain authoritative instead of being reduced");

    require(!koi::detail::SearchPolicy::late_move(
                8, 4, 7, 0, false, false, true, false, false, false, false, true, false)
                 .candidate,
            "checking moves must not be LMR candidates");
    require(!koi::detail::SearchPolicy::late_move(
                8, 4, 7, 0, false, false, false, true, false, false, false, true, false)
                 .candidate,
            "captures must not be LMR candidates");
    require(!koi::detail::SearchPolicy::late_move(
                8, 4, 7, 0, false, false, false, false, false, false, true, true, false)
                 .candidate,
            "TT moves must not be LMR candidates");
}

void test_quiet_futility_policy_owns_exact_boundary() {
    require(koi::detail::SearchPolicy::quiet_futility(
                true, false, false, false, 1, -200, 1, 0),
            "a quiet phase-rich move below the futility bound must prune");
    require(!koi::detail::SearchPolicy::quiet_futility(
                true, false, false, false, 0, -100, 1, 0),
            "the first move must remain authoritative");
    require(!koi::detail::SearchPolicy::quiet_futility(
                true, true, false, false, 1, -100, 1, 0),
            "captures must not use quiet futility");
    require(!koi::detail::SearchPolicy::quiet_futility(
                false, false, false, false, 1, -100, 1, 0),
            "non-phase-rich nodes must not use this futility gate");
}

void test_quiescence_capture_policy_classifies_see_and_delta_prunes() {
    using Prune = koi::detail::QuiescenceCapturePrune;
    require(koi::detail::SearchPolicy::quiescence_capture(
                false, true, false, false, -1, 500, 0, 0) == Prune::static_exchange,
            "losing captures must use SEE pruning");
    require(koi::detail::SearchPolicy::quiescence_capture(
                false, true, false, false, 0, 500, -1000, 0) == Prune::delta,
            "insufficient-gain captures must use delta pruning");
    require(koi::detail::SearchPolicy::quiescence_capture(
                false, true, true, false, -1, 500, 0, 0) == Prune::none,
            "checking captures must remain in quiescence");
    require(koi::detail::SearchPolicy::quiescence_capture(
                true, true, false, false, -1, 500, 0, 0) == Prune::none,
            "evasion captures must remain in quiescence");
}

void test_ordering_tables_own_mutation_and_reset() {
    koi::detail::SearchOrderingTables tables;
    const koi::Move previous = require_move("e2e4");
    const koi::Move killer = require_move("g1f3");
    const koi::Move counter = require_move("g8f6");

    tables.record_quiet_cutoff(koi::Color::white, killer, 0, 4);
    tables.record_quiet_cutoff(koi::Color::black, counter, 4, 8, previous);
    require(tables.is_killer(killer, 0), "cutoff updates must record the killer move");
    require(tables.quiet_history_score(koi::Color::white, killer) > 0,
            "cutoff updates must raise quiet history");
    require(tables.is_proven_counter_move(koi::Color::black, previous, counter),
            "cutoff updates must prove the counter move");

    tables.record_quiet_fail(koi::Color::black, counter, 4, 8, previous);
    require(!tables.is_proven_counter_move(koi::Color::black, previous, counter),
            "an exact counter failure must clear counter confidence");

    tables.clear();
    require(!tables.is_killer(killer, 0) &&
                tables.quiet_history_score(koi::Color::white, killer) == 0,
            "clearing ordering tables must remove adaptive state");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"null move policy", test_null_move_policy_owns_window_and_reduction_rules},
        {"check extension policy", test_check_extension_policy_owns_timing_and_budget_rules},
        {"late move policy", test_late_move_policy_owns_gating_history_and_reduction},
        {"quiet futility policy", test_quiet_futility_policy_owns_exact_boundary},
        {"quiescence capture policy", test_quiescence_capture_policy_classifies_see_and_delta_prunes},
        {"ordering table ownership", test_ordering_tables_own_mutation_and_reset},
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
    return failures == 0 ? 0 : 1;
}
