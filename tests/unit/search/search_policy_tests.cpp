#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/search_ordering_tables.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_policy.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

koi::Move require_move(const std::string_view uci) {
    return koi::test::require_value(koi::Move::parse_uci(uci), "test move must parse");
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

void test_dynamic_null_move_owns_reduction_and_verification_depth() {
    using koi::detail::SearchPolicy;

    const auto deep_candidate =
        SearchPolicy::dynamic_null_move(16, 20, 21, 600, false, true, false, false);
    require(deep_candidate.eligible && deep_candidate.verify,
            "a deep eligible null fail-high must be verified");
    require(deep_candidate.reduction == 7 + 16 / 3 + (600 - 21) / 256,
            "the dynamic null reduction must keep the Stockfish depth/excess shape");

    const auto shallow = SearchPolicy::dynamic_null_move(15, 20, 21, 600, false, true, false, false);
    require(shallow.eligible && !shallow.verify,
            "null verification must stay off below the deep-search threshold");

    require(!SearchPolicy::dynamic_null_move(16, 20, 21, -400, false, true, false, false).eligible,
            "a static evaluation below the confidence floor must reject null move");
    require(!SearchPolicy::dynamic_null_move(16, 20, 21, 600, false, true, false, true).eligible,
            "a pawn endgame must reject null move");
    require(!SearchPolicy::dynamic_null_move(16, 20, 21, 600, true, true, false, false).eligible,
            "checked nodes must reject null move");
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
    const auto ordinary_gate = koi::detail::SearchPolicy::late_move_gate(
        8, 4, 0, 0, false, false, false, false, false, false, false);
    require(ordinary_gate.candidate && !ordinary_gate.high_history_exclusion,
            "an ordinary late quiet move must pass the late-move gate");

    const auto ordinary = koi::detail::SearchPolicy::dynamic_late_move(
        8, 4, 7, 0, 0, 0, false, false, false, false, false, false, false, true, false,
        false, false, true, false, 0, true, false);
    require(ordinary.candidate && !ordinary.high_history_exclusion && ordinary.reduced,
            "an ordinary late quiet move must be reduced");
    require(ordinary.reduction == 1, "the baseline late move reduction must be one ply");

    const auto deep = koi::detail::SearchPolicy::dynamic_late_move(
        12, 20, 11, 0, 0, 0, false, false, false, false, false, false, false, true, false,
        false, false, true, false, 0, true, false);
    require(deep.candidate && deep.reduction == 3 && deep.reduced,
            "deep late moves must receive the configured depth/move reduction");

    const auto high_history_gate = koi::detail::SearchPolicy::late_move_gate(
        8, 4, 128, 0, false, false, false, false, false, false, false);
    require(high_history_gate.candidate && high_history_gate.high_history_exclusion,
            "high-history moves must remain authoritative instead of being reduced");
    const auto high_history = koi::detail::SearchPolicy::dynamic_late_move(
        8, 4, 7, 128, 0, 0, false, false, false, false, false, false, false, true, false,
        false, false, true, false, 0, true, false);
    require(!high_history.reduced,
            "high-history moves must not be reduced even when they are late");

    require(!koi::detail::SearchPolicy::late_move_gate(
                8, 4, 0, 0, false, false, true, false, false, false, false)
                 .candidate,
            "checking moves must not be LMR candidates");
    require(!koi::detail::SearchPolicy::late_move_gate(
                8, 4, 0, -9'000, false, false, false, true, false, false, false)
                 .candidate,
            "captures with deeply negative capture history must not be LMR candidates");
    require(koi::detail::SearchPolicy::late_move_gate(
                8, 4, 0, 0, false, false, false, true, false, false, false)
                .candidate,
            "captures with neutral capture history may be LMR candidates");
    require(koi::detail::SearchPolicy::late_move_gate(
                8, 4, 0, 512, false, false, false, true, false, false, false)
                .candidate,
            "captures with positive capture history may be LMR candidates");
    require(!koi::detail::SearchPolicy::late_move_gate(
                8, 4, 0, 0, false, false, false, false, false, false, true)
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
                false, true, false, false, -75, 500, 0, 0) == Prune::static_exchange,
            "captures below the SEE floor must use SEE pruning");
    require(koi::detail::SearchPolicy::quiescence_capture(
                false, true, false, false, -1, 500, 0, 0) == Prune::none,
            "captures inside the SEE floor band must remain in quiescence");
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

void test_selective_root_mate_estimate_is_not_a_proven_loss() {
    koi::detail::SearchContext::RootVerification verification;
    verification.score = -99'998;
    verification.line_complete = true;
    require(!verification.proves_losing_mate(),
            "a completed selective mate estimate cannot prove an escape is lost");

    verification.authoritative = true;
    require(verification.proves_losing_mate(),
            "an authoritative negative mate score can exclude a lost escape");
    verification.score = -500;
    require(!verification.proves_losing_mate(),
            "an ordinary negative score is not a proven mate loss");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"null move policy", test_null_move_policy_owns_window_and_reduction_rules},
        {"dynamic null move policy", test_dynamic_null_move_owns_reduction_and_verification_depth},
        {"check extension policy", test_check_extension_policy_owns_timing_and_budget_rules},
        {"late move policy", test_late_move_policy_owns_gating_history_and_reduction},
        {"quiet futility policy", test_quiet_futility_policy_owns_exact_boundary},
        {"quiescence capture policy", test_quiescence_capture_policy_classifies_see_and_delta_prunes},
        {"ordering table ownership", test_ordering_tables_own_mutation_and_reset},
        {"root verification mate provenance", test_selective_root_mate_estimate_is_not_a_proven_loss},
    };

    return koi::test::run_tests(tests, argc, argv);
}
