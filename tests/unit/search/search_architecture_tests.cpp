#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/detail/root_coordinator.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_session.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/thread_stack.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

koi::Move require_move(std::string_view uci) {
    return koi::test::require_value(koi::Move::parse_uci(uci), "test move must parse");
}

void test_stack_is_fixed_capacity_and_restores_frames() {
    koi::detail::SearchStack stack;
    require(stack.capacity() == koi::detail::SearchStack::kCapacity,
            "search stack capacity must be explicit");
    require(stack.capacity() >= 64,
            "search stack must cover the configured search depth");
    auto& frame = stack.frame(7);
    frame.current_move = require_move("e2e4");
    frame.move_count = 3;
    stack.reset();
    require(stack.frame(7).current_move.is_no_move() && stack.frame(7).move_count == 0,
            "reset must clear transient per-ply state");
}

void test_root_ranking_is_score_then_stable_index() {
    std::vector<koi::detail::RootLine> lines(3);
    lines[0].completed = true;
    lines[0].score = 20;
    lines[0].stable_index = 2;
    lines[1].completed = true;
    lines[1].score = 20;
    lines[1].stable_index = 0;
    lines[2].completed = true;
    lines[2].score = 30;
    lines[2].stable_index = 1;
    const auto ranked = koi::detail::RootCoordinator::rank(lines);
    require(ranked == std::vector<std::size_t>{2, 1, 0},
            "root ranking must be deterministic for equal scores");
}

void test_session_snapshot_and_completion_are_single_owner_operations() {
    koi::GameState root = koi::GameState::startpos();
    koi::SearchLimits limits;
    limits.depth = 2;
    koi::SearchOptions options;
    options.generation = 41;
    auto session = std::make_shared<koi::detail::SearchSession>(
        std::move(root), limits, options);
    require(session->generation() == 41,
            "session must own the request generation");
    require(session->root().position_key() != 0,
            "session must own the root snapshot");

    int completions = 0;
    koi::SearchEventSink sink;
    sink.on_complete = [&completions](const koi::SearchResult&) { ++completions; };
    koi::SearchResult result;
    (void)session->publish_completion(sink, result);
    (void)session->publish_completion(sink, result);
    require(completions == 1,
            "session must publish completion exactly once");
}

void test_search_context_exposes_fixed_stack_boundary() {
    require(koi::detail::SearchContext::stack_capacity() ==
                koi::detail::SearchStack::kCapacity,
            "search context must own the fixed-capacity search stack");
}

// Consumes a real stack frame per level so the worker body cannot be collapsed
// into a leaf call. The depth exceeds Darwin's 512 KiB pthread default, so on
// Apple the case only passes because spawn_worker_thread reserves the explicit
// worker stack; Linux (8 MiB RLIMIT_STACK) and Windows (16 MiB image reserve)
// cover the same depth with their ordinary worker budgets.
int consume_worker_stack(const int depth) {
    volatile char frame[64] = {};
    frame[0] = static_cast<char>(depth);
    if (depth <= 0) {
        return frame[0];
    }
    return consume_worker_stack(depth - 1) + 1;
}

void test_worker_thread_spawns_joinable_workers_with_stack_headroom() {
    require(koi::detail::kWorkerStackBytes >= std::size_t{16} * 1024 * 1024,
            "worker stack budget must not shrink below the Linux/Windows 16 MiB");
    constexpr int kWorkerCount = 4;
    // At least 1 MiB of touched frames, comfortably above the 512 KiB default.
    constexpr int kRecursionDepth = 16'384;
    std::atomic<int> completed{0};
    std::vector<koi::detail::WorkerThread> workers;
    workers.reserve(kWorkerCount);
    try {
        for (int index = 0; index < kWorkerCount; ++index) {
            auto worker = koi::detail::spawn_worker_thread([&completed, index] {
                if (consume_worker_stack(kRecursionDepth + index) > 0) {
                    completed.fetch_add(1, std::memory_order_relaxed);
                }
            });
            require(worker.joinable(), "spawned worker must be joinable");
            require(worker.get_id() != std::this_thread::get_id(),
                    "spawned worker must run on another thread");
            workers.push_back(std::move(worker));
            require(!worker.joinable(), "moved-from worker must not be joinable");
        }
    } catch (...) {
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
    for (auto& worker : workers) {
        worker.join();
        require(!worker.joinable(), "joined worker must not be joinable");
    }
    require(completed.load(std::memory_order_relaxed) == kWorkerCount,
            "every spawned worker must complete its work");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"fixed search stack", test_stack_is_fixed_capacity_and_restores_frames},
        {"deterministic root ranking", test_root_ranking_is_score_then_stable_index},
        {"session ownership", test_session_snapshot_and_completion_are_single_owner_operations},
        {"context stack boundary", test_search_context_exposes_fixed_stack_boundary},
        {"worker thread spawn", test_worker_thread_spawns_joinable_workers_with_stack_headroom},
    };
    return koi::test::run_tests(tests, argc, argv);
}
