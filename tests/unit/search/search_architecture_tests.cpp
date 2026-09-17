#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/root_coordinator.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_session.hpp"
#include "koi/detail/search_stack.hpp"

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

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"fixed search stack", test_stack_is_fixed_capacity_and_restores_frames},
        {"deterministic root ranking", test_root_ranking_is_score_then_stable_index},
        {"session ownership", test_session_snapshot_and_completion_are_single_owner_operations},
        {"context stack boundary", test_search_context_exposes_fixed_stack_boundary},
    };
    return koi::test::run_tests(tests, argc, argv);
}
