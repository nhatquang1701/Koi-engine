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
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
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
    };
    return koi::test::run_tests(tests, argc, argv);
}
