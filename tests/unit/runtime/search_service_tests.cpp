// Focused unit coverage for SearchService/SearchHandle lifecycle: start/join,
// stop of unbounded searches, ponderhit conversion, evaluator replacement and
// hash controls.  The heavy search correctness remains in koi_search_tests;
// this suite pins the service boundary itself.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/detail/search_constants.hpp"
#include "koi/search_service.hpp"
#include "koi/search_types.hpp"
#include "koi_test_support.hpp"

namespace {

using namespace std::chrono_literals;

using koi::Color;
using koi::Evaluator;
using koi::GameState;
using koi::SearchEventSink;
using koi::SearchInfo;
using koi::SearchLimits;
using koi::SearchResult;
using koi::SearchService;
using koi::SearchOptions;
using koi::TablebaseProbeResult;
using koi::test::require;

// Counts evaluations so the service-level evaluator swap can be observed from
// the test without running a long search.
class CountingEvaluator final : public Evaluator {
public:
    int evaluate(const GameState&, Color) const override {
        evaluations_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    [[nodiscard]] std::uint64_t evaluations() const noexcept {
        return evaluations_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<std::uint64_t> evaluations_{0};
};

// Records the advisory make/unmake notifications the search forwards to each
// private evaluation worker so hook wiring can be asserted end to end.
struct HookCounters {
    std::atomic<int> makes{0};
    std::atomic<int> unmakes{0};
    std::atomic<int> null_makes{0};
    std::atomic<int> null_unmakes{0};
};

class HookCountingWorker final : public koi::EvaluatorWorker {
public:
    explicit HookCountingWorker(std::shared_ptr<HookCounters> counters)
        : counters_(std::move(counters)) {}

    int evaluate(const GameState&, Color) override {
        return 0;
    }

    void on_make_move(const GameState&, const koi::MoveMetadata&, int, std::uint64_t) override {
        counters_->makes.fetch_add(1, std::memory_order_relaxed);
    }

    void on_unmake_move(int) override {
        counters_->unmakes.fetch_add(1, std::memory_order_relaxed);
    }

    void on_make_null_move(const GameState&, int, std::uint64_t) override {
        counters_->null_makes.fetch_add(1, std::memory_order_relaxed);
    }

    void on_unmake_null_move(int) override {
        counters_->null_unmakes.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::shared_ptr<HookCounters> counters_;
};

class HookCountingEvaluator final : public Evaluator {
public:
    int evaluate(const GameState&, Color) const override {
        return 0;
    }

    std::unique_ptr<koi::EvaluatorWorker> create_worker() const override {
        return std::make_unique<HookCountingWorker>(counters_);
    }

    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }

    std::shared_ptr<HookCounters> counters_ = std::make_shared<HookCounters>();
};

[[nodiscard]] bool wait_for(const std::function<bool()>& predicate,
                            const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void test_depth_limited_search_completes_once() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    GameState root = GameState::startpos();

    std::atomic<int> completions{0};
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&completions, &result](const SearchResult& completed) {
        result = completed;
        completions.fetch_add(1, std::memory_order_relaxed);
    };

    SearchLimits limits;
    limits.depth = 2;
    auto handle = service.start(root, limits, sink);
    handle.wait();

    require(!handle.running(), "a joined handle must not report running");
    require(completions.load() == 1, "a depth-limited search must complete exactly once");
    require(result.has_value() && result->completed, "the search must complete authoritatively");
    require(result->best_move.has_value(), "a completed search must return a best move");
    require(root.is_legal(*result->best_move), "the returned move must be legal in the root");
}

void test_stop_ends_an_infinite_search() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    GameState root = GameState::startpos();

    std::atomic<int> info_count{0};
    std::atomic<int> completions{0};
    SearchEventSink sink;
    sink.on_info = [&info_count](const SearchInfo&) {
        info_count.fetch_add(1, std::memory_order_relaxed);
    };
    sink.on_complete = [&completions](const SearchResult&) {
        completions.fetch_add(1, std::memory_order_relaxed);
    };

    SearchLimits limits;
    limits.infinite = true;
    auto handle = service.start(root, limits, sink);

    require(wait_for([&info_count] { return info_count.load() > 0; }, 10s),
            "an infinite search must emit at least one info report");
    handle.stop();
    handle.wait();

    require(!handle.running(), "a stopped handle must join");
    require(completions.load() == 1, "a stopped search must still publish exactly one completion");
}

void test_set_evaluator_applies_to_the_next_search() {
    auto first = std::make_shared<CountingEvaluator>();
    SearchService service(first, {}, 16);
    GameState root = GameState::startpos();

    std::atomic<int> completions{0};
    SearchEventSink sink;
    sink.on_complete = [&completions](const SearchResult&) {
        completions.fetch_add(1, std::memory_order_relaxed);
    };

    SearchLimits limits;
    limits.depth = 1;
    auto first_handle = service.start(root, limits, sink);
    first_handle.wait();
    require(first->evaluations() > 0, "the initial evaluator must serve the first search");

    auto second = std::make_shared<CountingEvaluator>();
    service.set_evaluator(second);
    auto second_handle = service.start(root, limits, sink);
    second_handle.wait();

    require(second->evaluations() > 0, "the replacement evaluator must serve the next search");
    require(completions.load() == 2, "both searches must complete exactly once");
}

void test_running_search_keeps_its_evaluator() {
    auto first = std::make_shared<CountingEvaluator>();
    SearchService service(first, {}, 16);
    GameState root = GameState::startpos();

    std::atomic<int> info_count{0};
    SearchEventSink sink;
    sink.on_info = [&info_count](const SearchInfo&) {
        info_count.fetch_add(1, std::memory_order_relaxed);
    };

    SearchLimits limits;
    limits.infinite = true;
    auto handle = service.start(root, limits, sink);
    require(wait_for([&info_count] { return info_count.load() > 0; }, 10s),
            "the search must start evaluating before the swap");

    auto replacement = std::make_shared<CountingEvaluator>();
    service.set_evaluator(replacement);
    handle.stop();
    handle.wait();

    require(replacement->evaluations() == 0,
            "a running search must keep the evaluator it started with");
}

void test_hash_controls_are_observable() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    require(service.hash_size_mb() == 16, "the service must report the configured hash size");
    require(service.hashfull_permill() <= 1000, "hashfull must be a permill value");

    service.clear_hash();
    (void)service.set_hash_size_mb(4);
    require(service.hash_size_mb() == 4, "hash resizing must be observable");

    (void)service.set_hash_size_mb(0);
    require(service.hash_size_mb() == 1, "hash requests below the minimum must clamp to one megabyte");
}

void test_search_without_a_sink_still_completes() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 8);
    SearchLimits limits;
    limits.depth = 1;

    auto handle = service.start(GameState::startpos(), limits);
    handle.wait();
    require(!handle.running(), "a search with no event sink must still finish");
}

void test_request_ponderhit_converts_a_running_ponder_search() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    GameState root = GameState::startpos();

    std::atomic<int> completions{0};
    SearchEventSink sink;
    sink.on_complete = [&completions](const SearchResult&) {
        completions.fetch_add(1, std::memory_order_relaxed);
    };

    SearchLimits limits;
    limits.ponder = true;
    limits.depth = 2;
    auto handle = service.start(root, limits, sink);
    require(handle.running(), "a ponder search must be running before the hit");

    SearchLimits converted = limits;
    converted.ponder = false;
    handle.request_ponderhit(converted);

    const bool finished = wait_for([&handle] { return !handle.running(); }, 20s);
    if (!finished) {
        handle.stop();
    }
    handle.wait();

    require(finished, "a ponderhit request must let the bounded search finish without a stop");
    require(completions.load() == 1, "the converted search must publish exactly one completion");
}

void test_search_forwards_advisory_incremental_hooks() {
    auto evaluator = std::make_shared<HookCountingEvaluator>();
    SearchService service(evaluator, {}, 16);
    GameState root = GameState::startpos();

    SearchEventSink sink;
    SearchLimits limits;
    limits.depth = 4;
    auto handle = service.start(root, limits, sink);
    handle.wait();

    require(evaluator->counters_->makes.load(std::memory_order_relaxed) > 0,
            "a search must notify move makes to the private worker: makes=" +
                std::to_string(evaluator->counters_->makes.load(std::memory_order_relaxed)) +
                " unmakes=" +
                std::to_string(evaluator->counters_->unmakes.load(std::memory_order_relaxed)) +
                " nulls=" +
                std::to_string(evaluator->counters_->null_makes.load(std::memory_order_relaxed)));
    require(evaluator->counters_->makes.load(std::memory_order_relaxed) ==
                evaluator->counters_->unmakes.load(std::memory_order_relaxed),
            "search make and unmake notifications must balance");
    require(evaluator->counters_->null_makes.load(std::memory_order_relaxed) ==
                evaluator->counters_->null_unmakes.load(std::memory_order_relaxed),
            "search null-move notifications must balance");
}

// Phase B interior tablebase plumbing: a diagnostic hook stands in for a real
// tablebase so the cutoff, provenance and gating can be asserted without any
// tablebase assets on disk.  SyzygyInteriorDepth = 0 must stay byte-identical.
[[nodiscard]] std::optional<koi::TablebaseProbeResult> decisive_loss_hook(
    std::atomic<int>* calls, const GameState&, int) {
    calls->fetch_add(1, std::memory_order_relaxed);
    return std::optional<koi::TablebaseProbeResult>{
        koi::TablebaseProbeResult{0, -1}};
}

void test_interior_tablebase_hook_cuts_off_a_decisive_score() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    const GameState root = koi::test::require_value(
        GameState::from_fen("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1"),
        "the interior tablebase hook fixture must parse");

    std::atomic<int> calls{0};
    SearchOptions options;
    options.syzygy_interior_depth = 1;
    options.tablebase_probe_hook = [&calls](const GameState& state, int depth) {
        return decisive_loss_hook(&calls, state, depth);
    };

    SearchLimits limits;
    limits.depth = 3;
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&result](const SearchResult& completed) { result = completed; };
    auto handle = service.start(root, limits, sink, options);
    handle.wait();

    require(result.has_value() && result->completed, "the hooked search must complete");
    require(calls.load() > 0, "the interior hook must be consulted inside the tree");
    require(result->stats.tbhits > 0,
            "a decisive hook cutoff must record a tablebase hit [calls=" +
                std::to_string(calls.load()) + " tbhits=" +
                std::to_string(result->stats.tbhits) + " score=" +
                std::to_string(result->score_cp) + " depth=" +
                std::to_string(result->completed_depth) + "]");
    require(result->score_cp == koi::detail::kTablebaseInteriorWinScore,
            "a decisive hook cutoff must publish the tablebase win score");
    require(!result->mate.has_value(),
            "an interior tablebase score below the mate threshold must not advertise mate");
}

void test_interior_tablebase_hook_ignores_non_decisive_results() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    const GameState root = koi::test::require_value(
        GameState::from_fen("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1"),
        "the interior tablebase hook fixture must parse");

    std::atomic<int> calls{0};
    SearchOptions options;
    options.syzygy_interior_depth = 1;
    options.tablebase_probe_hook = [&calls](const GameState&, int) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return std::optional<koi::TablebaseProbeResult>{
            koi::TablebaseProbeResult{1234, std::nullopt}};
    };

    SearchLimits limits;
    limits.depth = 3;
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&result](const SearchResult& completed) { result = completed; };
    auto handle = service.start(root, limits, sink, options);
    handle.wait();

    require(result.has_value() && result->completed, "the hooked search must complete");
    require(calls.load() > 0, "the interior hook must be consulted inside the tree");
    require(result->stats.tbhits == 0,
            "a non-decisive hook result must not record a tablebase hit");
    require(result->score_cp > -koi::detail::kTablebaseInteriorWinScore,
            "a non-decisive hook result must not replace the search score");
}

void test_interior_tablebase_hook_ignores_zero_mate_draws() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    const GameState root = koi::test::require_value(
        GameState::from_fen("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1"),
        "the interior tablebase hook fixture must parse");

    std::atomic<int> calls{0};
    SearchOptions options;
    options.syzygy_interior_depth = 1;
    options.tablebase_probe_hook = [&calls](const GameState&, int) {
        calls.fetch_add(1, std::memory_order_relaxed);
        return std::optional<koi::TablebaseProbeResult>{
            koi::TablebaseProbeResult{0, 0}};
    };

    SearchLimits limits;
    limits.depth = 3;
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&result](const SearchResult& completed) { result = completed; };
    auto handle = service.start(root, limits, sink, options);
    handle.wait();

    require(result.has_value() && result->completed, "the hooked search must complete");
    require(calls.load() > 0, "the interior hook must be consulted inside the tree");
    require(result->stats.tbhits == 0,
            "draw-class hook results must not record a tablebase hit");
    require(result->score_cp > -koi::detail::kTablebaseInteriorWinScore,
            "draw-class hook results must not replace the search score");
}

void test_interior_tablebase_hook_respects_the_fifty_move_rule() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    const GameState root = koi::test::require_value(
        GameState::from_fen("8/8/8/4k3/8/8/8/4K2R w - - 20 40"),
        "the fifty-move hook fixture must parse");

    std::atomic<int> calls{0};
    SearchOptions options;
    options.syzygy_interior_depth = 1;
    options.tablebase_probe_hook = [&calls](const GameState& state, int depth) {
        return decisive_loss_hook(&calls, state, depth);
    };

    SearchLimits limits;
    limits.depth = 3;
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&result](const SearchResult& completed) { result = completed; };
    auto handle = service.start(root, limits, sink, options);
    handle.wait();

    require(result.has_value() && result->completed, "the hooked search must complete");
    require(calls.load() == 0,
            "a nonzero halfmove clock must suppress the fifty-move-rule probe [calls=" +
                std::to_string(calls.load()) + " tbhits=" +
                std::to_string(result->stats.tbhits) + "]");
    require(result->stats.tbhits == 0,
            "a suppressed probe must not record a tablebase hit");
}

void test_interior_tablebase_hook_exceptions_are_swallowed() {
    SearchService service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    const GameState root = koi::test::require_value(
        GameState::from_fen("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1"),
        "the interior tablebase hook fixture must parse");

    SearchOptions options;
    options.syzygy_interior_depth = 1;
    options.tablebase_probe_hook = [](const GameState&, int) -> std::optional<koi::TablebaseProbeResult> {
        throw std::runtime_error("diagnostic hook failure");
    };

    SearchLimits limits;
    limits.depth = 2;
    std::optional<SearchResult> result;
    SearchEventSink sink;
    sink.on_complete = [&result](const SearchResult& completed) { result = completed; };
    auto handle = service.start(root, limits, sink, options);
    handle.wait();

    require(result.has_value() && result->completed && result->best_move.has_value(),
            "a thrown hook error must not abort the search");
    require(result->stats.tbhits == 0,
            "a thrown hook error must not record a tablebase hit");
}

void test_interior_tablebase_default_off_is_byte_identical() {
    const GameState root = GameState::startpos();
    SearchLimits limits;
    limits.depth = 4;

    std::optional<SearchResult> reference;
    SearchEventSink reference_sink;
    reference_sink.on_complete = [&reference](const SearchResult& completed) {
        reference = completed;
    };
    SearchService reference_service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    auto reference_handle = reference_service.start(root, limits, reference_sink);
    reference_handle.wait();
    require(reference.has_value(), "the reference search must complete");

    std::atomic<int> calls{0};
    SearchOptions options;
    options.tablebase_probe_hook = [&calls](const GameState& state, int depth) {
        return decisive_loss_hook(&calls, state, depth);
    };
    std::optional<SearchResult> hooked;
    SearchEventSink hooked_sink;
    hooked_sink.on_complete = [&hooked](const SearchResult& completed) { hooked = completed; };
    SearchService hooked_service(std::make_shared<koi::ClassicalEvaluator>(), {}, 16);
    auto hooked_handle = hooked_service.start(root, limits, hooked_sink, options);
    hooked_handle.wait();
    require(hooked.has_value(), "the hooked search must complete");

    require(calls.load() == 0,
            "an interior hook without a configured interior depth must never run");
    require(hooked->stats.nodes == reference->stats.nodes &&
                hooked->stats.qnodes == reference->stats.qnodes,
            "the default SyzygyInteriorDepth = 0 must preserve the node counts");
    require(hooked->score_cp == reference->score_cp &&
                hooked->best_move == reference->best_move,
            "the default SyzygyInteriorDepth = 0 must preserve the result");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests = {
        {"search service depth search", test_depth_limited_search_completes_once},
        {"search service stop", test_stop_ends_an_infinite_search},
        {"search service evaluator swap", test_set_evaluator_applies_to_the_next_search},
        {"search service evaluator pinning", test_running_search_keeps_its_evaluator},
        {"search service hash controls", test_hash_controls_are_observable},
        {"search service sinkless search", test_search_without_a_sink_still_completes},
        {"search service ponderhit conversion", test_request_ponderhit_converts_a_running_ponder_search},
        {"search service incremental hooks", test_search_forwards_advisory_incremental_hooks},
        {"interior tablebase hook cutoff", test_interior_tablebase_hook_cuts_off_a_decisive_score},
        {"interior tablebase hook non-decisive", test_interior_tablebase_hook_ignores_non_decisive_results},
        {"interior tablebase hook zero mate", test_interior_tablebase_hook_ignores_zero_mate_draws},
        {"interior tablebase hook fifty move", test_interior_tablebase_hook_respects_the_fifty_move_rule},
        {"interior tablebase hook exception", test_interior_tablebase_hook_exceptions_are_swallowed},
        {"interior tablebase default off", test_interior_tablebase_default_off_is_byte_identical},
    };
    return koi::test::run_tests(tests, argc, argv);
}
