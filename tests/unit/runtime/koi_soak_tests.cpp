// Stability soak coverage for the hardening work: long replays past the
// snapshot window, many-thread searches, hash resizes under a live search, and
// repeated ponder cycles.  The suite is deliberately bounded to a few seconds
// so the sanitizer and no-retry flake jobs can run it; every case asserts only
// invariants that must hold under any scheduling (a legal best move, exactly
// one completion, no reported failure), never a timing or a node count.
//
// These cases complement the targeted unit tests: they drive the real
// lifecycle through the public service seam with real threads and real wall
// clock, which is where the retired-storage and session-lifetime bugs
// surfaced.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/move.hpp"
#include "koi/search_service.hpp"
#include "koi/search_types.hpp"

#include "koi_test_support.hpp"

namespace {

using namespace std::chrono_literals;

using koi::ClassicalEvaluator;
using koi::GameState;
using koi::Move;
using koi::SearchEventSink;
using koi::SearchHandle;
using koi::SearchInfo;
using koi::SearchLimits;
using koi::SearchOptions;
using koi::SearchResult;
using koi::SearchService;

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

// Counts completions and keeps the last result, exactly like the controller
// consumes the search event channel.
struct CompletionSpy {
    std::atomic<int> completions{0};
    std::optional<SearchResult> result;

    [[nodiscard]] SearchEventSink sink() {
        SearchEventSink events;
        events.on_complete = [this](const SearchResult& completed) {
            result = completed;
            completions.fetch_add(1, std::memory_order_relaxed);
        };
        return events;
    }
};

[[nodiscard]] SearchService make_service(const std::size_t hash_mb = 16) {
    return SearchService(std::make_shared<ClassicalEvaluator>(), {}, hash_mb);
}

// The replay stays inside the native snapshot window and then crosses it: the
// position is 320 plies deep when the search starts, which is well past the
// 256-ply history array the rules core keeps.
void test_long_replay_keeps_accepting_moves_and_searching() {
    GameState root = GameState::startpos();
    static constexpr const char* kShuffle[] = {"g1f3", "g8f6", "f3g1", "f6g8"};
    // The shuffle crosses the 256-ply snapshot window; the final pawn pair then
    // leaves the root on a fresh, non-repeated position so the search below is
    // an ordinary search rather than a repetition shortcut.
    static constexpr const char* kFinish[] = {"e2e4", "e7e5"};
    for (int round = 0; round < 80; ++round) {
        for (const char* text : kShuffle) {
            const std::optional<Move> move = Move::parse_uci(text);
            koi::test::require(move.has_value(), "a replay shuffle move must parse");
            koi::test::require(root.make_move(*move),
                               "a long replay must keep accepting legal moves past the "
                               "history window");
        }
    }
    for (const char* text : kFinish) {
        const std::optional<Move> move = Move::parse_uci(text);
        koi::test::require(move.has_value(), "a replay finish move must parse");
        koi::test::require(root.make_move(*move), "the replay finish must stay legal");
    }

    SearchService service = make_service();
    SearchOptions options;
    options.threads = 8;
    SearchLimits limits;
    limits.depth = 4;
    for (int attempt = 0; attempt < 2; ++attempt) {
        CompletionSpy spy;
        SearchHandle handle = service.start(root, limits, spy.sink(), options);
        handle.wait();
        koi::test::require(spy.completions.load() == 1,
                           "a depth-limited replay search must complete exactly once");
        koi::test::require(spy.result.has_value() && spy.result->best_move.has_value(),
                           "a replay search must return a best move");
        koi::test::require(spy.result->completed_depth >= 2,
                           "a replay search must complete real iterations");
        koi::test::require(root.is_legal(*spy.result->best_move),
                           "a replay search must return a legal best move");
        koi::test::require(!spy.result->failed,
                           "a replay search must not report a helper failure");
    }
}

// Resizing the table while four workers read it is the regression case for the
// retired-storage lifetime fix; the search must survive the churn and still
// publish one legal completion.
void test_hash_resize_during_a_live_search() {
    SearchService service = make_service(8);
    GameState root = GameState::startpos();

    std::atomic<int> infos{0};
    CompletionSpy spy;
    SearchEventSink events = spy.sink();
    events.on_info = [&infos](const SearchInfo&) {
        infos.fetch_add(1, std::memory_order_relaxed);
    };

    SearchOptions options;
    options.threads = 4;
    SearchLimits limits;
    limits.infinite = true;
    SearchHandle handle = service.start(root, limits, events, options);
    koi::test::require(wait_for([&infos] { return infos.load() > 0; }, 10s),
                       "the resize soak search must report progress");

    for (int resize = 0; resize < 12; ++resize) {
        (void)service.set_hash_size_mb(resize % 2 == 0 ? 1 : 4);
        std::this_thread::sleep_for(5ms);
    }

    handle.stop();
    handle.wait();
    koi::test::require(spy.completions.load() == 1,
                       "a resized search must complete exactly once");
    koi::test::require(spy.result.has_value() && spy.result->best_move.has_value() &&
                           root.is_legal(*spy.result->best_move),
                       "a resized search must return a legal best move");
}

// A ponder search must park after its depth limit and publish exactly one
// answer once stopped, never a bestmove while it is still pondering.
void test_ponder_cycles_stop_exactly_once() {
    SearchService service = make_service();
    GameState root = GameState::startpos();

    SearchOptions options;
    options.threads = 2;
    for (int cycle = 0; cycle < 4; ++cycle) {
        CompletionSpy spy;
        SearchLimits limits;
        limits.depth = 4;
        limits.ponder = true;
        SearchHandle handle = service.start(root, limits, spy.sink(), options);
        koi::test::require(spy.completions.load() == 0,
                           "a pondering search must not publish before stop");
        std::this_thread::sleep_for(30ms);
        handle.stop();
        handle.wait();
        koi::test::require(spy.completions.load() == 1,
                           "a stopped ponder must publish exactly one completion");
        koi::test::require(spy.result.has_value() && spy.result->best_move.has_value() &&
                               root.is_legal(*spy.result->best_move),
                           "a stopped ponder must return a legal best move");
    }
}

// Cycling the thread count through the pool boundary exercises pool
// construction, helper startup, and teardown repeatedly in one process.
void test_thread_count_sweep_keeps_every_result_legal() {
    SearchService service = make_service(32);
    GameState root = GameState::startpos();

    for (const std::size_t threads : {std::size_t{1}, std::size_t{2}, std::size_t{4},
                                      std::size_t{8}, std::size_t{16}}) {
        CompletionSpy spy;
        SearchOptions options;
        options.threads = threads;
        SearchLimits limits;
        limits.depth = 3;
        SearchHandle handle = service.start(root, limits, spy.sink(), options);
        handle.wait();
        koi::test::require(spy.completions.load() == 1,
                           "a threaded search must complete exactly once");
        koi::test::require(spy.result.has_value() && spy.result->best_move.has_value() &&
                               root.is_legal(*spy.result->best_move),
                           "a threaded search must return a legal best move");
        koi::test::require(!spy.result->failed,
                           "a threaded search must not report a failure");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"long replay soak", test_long_replay_keeps_accepting_moves_and_searching},
        {"hash resize soak", test_hash_resize_during_a_live_search},
        {"ponder cycle soak", test_ponder_cycles_stop_exactly_once},
        {"thread sweep soak", test_thread_count_sweep_keeps_every_result_legal},
    };
    return koi::test::run_tests(tests, argc, argv);
}
