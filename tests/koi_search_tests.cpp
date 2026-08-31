#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace {

using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "test FEN must construct a game state");
    return *state;
}

struct CompletedSearch {
    mutable std::mutex mutex;
    std::optional<koi::SearchResult> result;
    std::uint32_t completions = 0;

    koi::SearchEventSink sink() {
        return {.on_complete = [this](const koi::SearchResult& completed) {
                    std::lock_guard lock(mutex);
                    result = completed;
                    ++completions;
                }};
    }

    koi::SearchResult take_result() {
        std::lock_guard lock(mutex);
        require(result.has_value(), "search must report a result");
        require(completions == 1, "search must report exactly one final result");
        return *result;
    }

    std::uint32_t completion_count() const {
        std::lock_guard lock(mutex);
        return completions;
    }
};

koi::SearchResult search(koi::SearchService& service, koi::GameState root, koi::SearchLimits limits) {
    CompletedSearch completed;
    koi::SearchHandle handle = service.start(std::move(root), limits, completed.sink());
    handle.wait();
    return completed.take_result();
}

void test_evaluator_returns_material_and_pst_from_requested_perspective() {
    koi::ClassicalEvaluator evaluator;
    const koi::GameState queen_up = require_state("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    const koi::GameState pawn_home = require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    const koi::GameState pawn_advanced = require_state("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");

    const int queen_score = evaluator.evaluate(queen_up, koi::Color::white);
    require(queen_score >= 850, "a white queen advantage must evaluate as strongly positive");
    require(evaluator.evaluate(queen_up, koi::Color::black) == -queen_score,
            "black perspective must negate the white perspective score");
    require(evaluator.evaluate(pawn_advanced, koi::Color::white) > evaluator.evaluate(pawn_home, koi::Color::white),
            "a centrally advanced white pawn must receive a positive PST improvement");
}

void test_time_manager_applies_move_time_and_clock_limits() {
    koi::SearchLimits move_time;
    move_time.movetime = 100ms;
    move_time.white_clock = koi::ClockLimit{10s, 1s};
    koi::TimeManager fixed(move_time, koi::Color::white);
    require(fixed.time_budget().has_value(), "movetime must create a time budget");
    require(*fixed.time_budget() <= 100ms, "movetime must take precedence over clock allocation");

    koi::SearchLimits clock;
    clock.white_clock = koi::ClockLimit{10s, 1s};
    clock.moves_to_go = 20;
    koi::TimeManager allocated(clock, koi::Color::white);
    require(allocated.time_budget().has_value(), "side-to-move clock must create a time budget");
    require(*allocated.time_budget() > 0ms && *allocated.time_budget() < 10s,
            "clock allocation must retain a safety margin");

    koi::SearchLimits nodes;
    nodes.nodes = 5;
    koi::TimeManager node_limited(nodes, koi::Color::white);
    require(!node_limited.should_stop(4), "node limit must allow work below its boundary");
    require(node_limited.should_stop(5), "node limit must stop at its boundary");

    koi::SearchLimits infinite;
    infinite.infinite = true;
    infinite.movetime = 1ms;
    koi::TimeManager unbounded(infinite, koi::Color::white);
    require(!unbounded.time_budget().has_value(), "infinite search must ignore a time budget");
}

void test_terminal_roots_return_mate_or_stalemate_scores() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    const koi::SearchResult mate = search(
        service, require_state("7k/6Q1/5K2/8/8/8/8/8 b - - 0 1"), limits);
    require(!mate.best_move.has_value(), "a checkmated root must not return a move");
    require(mate.score_cp < -90000, "a checkmated side must receive a terminal losing score");
    require(mate.mate.has_value() && *mate.mate <= 0, "a checkmated side must report mate");

    const koi::SearchResult stalemate = search(
        service, require_state("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"), limits);
    require(!stalemate.best_move.has_value(), "a stalemated root must not return a move");
    require(stalemate.score_cp == 0 && !stalemate.mate.has_value(),
            "a stalemate must receive a draw score without mate");
}

void test_fixed_depth_search_is_deterministic_and_legal() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;
    const koi::GameState root = koi::GameState::startpos();

    const koi::SearchResult first = search(service, root, limits);
    const koi::SearchResult second = search(service, root, limits);

    require(first.completed_depth == 3 && second.completed_depth == 3,
            "fixed-depth searches must complete the requested depth");
    require(first.best_move.has_value() && second.best_move.has_value(),
            "non-terminal roots must return a best move");
    require(*first.best_move == *second.best_move && first.score_cp == second.score_cp,
            "identical roots and limits must produce identical results");
    require(root.is_legal(*first.best_move), "search must only return legal root moves");
}

void test_infinite_search_runs_until_stopped_and_completes_once() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.infinite = true;
    limits.depth = 1;
    CompletedSearch completed;

    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink());
    std::this_thread::sleep_for(50ms);
    require(handle.running(), "infinite search must remain running instead of completing its depth limit");
    require(completed.completion_count() == 0, "infinite search must not complete before cancellation");

    handle.stop();
    handle.wait();
    require(!handle.running(), "stopped infinite search must join its worker");
    const koi::SearchResult result = completed.take_result();
    require(result.best_move.has_value(), "stopped non-terminal infinite search must retain a legal fallback move");
}

void test_service_hash_configuration_survives_default_start_and_non_default_override() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 1;

    service.set_hash_size_mb(1);
    require(service.hash_size_mb() == 1, "service hash configuration must accept 1 MB");
    search(service, koi::GameState::startpos(), limits);
    require(service.hash_size_mb() == 1, "default SearchOptions must preserve the configured service hash size");

    CompletedSearch completed;
    koi::SearchOptions override;
    override.hash_mb = 2;
    koi::SearchHandle handle = service.start(koi::GameState::startpos(), limits, completed.sink(), override);
    handle.wait();
    completed.take_result();
    require(service.hash_size_mb() == 2, "a non-default per-start hash option must explicitly reconfigure the service hash");
}

void test_hash_configuration_clamps_to_uci_bounds_and_clear_discards_warmed_entries() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    koi::SearchLimits limits;
    limits.depth = 3;

    require(service.hash_size_mb() == 16, "SearchService must retain the 16 MB default hash size");
    const koi::SearchResult warmed = search(service, koi::GameState::startpos(), limits);
    require(warmed.stats.tt_hits > 0, "a completed iterative search must warm the transposition table");

    service.clear_hash();
    koi::SearchLimits one_ply;
    one_ply.depth = 1;
    const koi::SearchResult cleared = search(service, koi::GameState::startpos(), one_ply);
    require(cleared.stats.tt_hits == 0, "Clear Hash must remove entries used by a subsequent root search");

    service.set_hash_size_mb(0);
    require(service.hash_size_mb() == 1, "hash size must clamp to the UCI lower bound of 1 MB");
}

void test_transposition_table_stores_probes_and_clears_entries() {
    koi::TranspositionTable table;
    require(table.size_mb() == 16, "transposition table default must be 16 MB");
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");

    require(!table.probe(0).has_value(), "a fresh table must not report a zero-key entry");
    table.store(0, 6, 17, koi::TranspositionBound::exact, *move);
    const auto zero_key_entry = table.probe(0);
    require(zero_key_entry.has_value() && zero_key_entry->score == 17,
            "a stored zero key must probe successfully");
    table.clear();
    require(!table.probe(0).has_value(), "a cleared table must not report a zero-key entry");

    table.new_generation();
    table.store(0x0123456789abcdefULL, 7, 42, koi::TranspositionBound::exact, *move);
    const auto entry = table.probe(0x0123456789abcdefULL);
    require(entry.has_value(), "stored key must probe successfully");
    require(entry->depth == 7 && entry->score == 42 && entry->bound == koi::TranspositionBound::exact,
            "probe must retain depth, score, and bound");
    require(entry->best_move == *move, "probe must retain the best move");

    table.clear();
    require(!table.probe(0x0123456789abcdefULL).has_value(), "clear must remove a stored entry");
    table.set_size_mb(1);
    require(table.size_mb() == 1, "hash configuration must accept the lower 1 MB bound");
}

void test_transposition_table_preserves_mate_distance_across_plies() {
    koi::TranspositionTable table;
    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "test move must parse");

    table.store(0x1111ULL, 6, 99'993, koi::TranspositionBound::exact, *move, 7);
    const auto winning_mate = table.probe(0x1111ULL, 3);
    require(winning_mate.has_value() && winning_mate->score == 99'997,
            "a winning mate score must be adjusted to the probing ply");

    table.store(0x2222ULL, 6, -99'993, koi::TranspositionBound::exact, *move, 7);
    const auto losing_mate = table.probe(0x2222ULL, 3);
    require(losing_mate.has_value() && losing_mate->score == -99'997,
            "a losing mate score must be adjusted to the probing ply");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"classical evaluator", test_evaluator_returns_material_and_pst_from_requested_perspective},
        {"time manager", test_time_manager_applies_move_time_and_clock_limits},
        {"terminal search", test_terminal_roots_return_mate_or_stalemate_scores},
        {"deterministic legal search", test_fixed_depth_search_is_deterministic_and_legal},
        {"infinite search lifecycle", test_infinite_search_runs_until_stopped_and_completes_once},
        {"service hash persistence", test_service_hash_configuration_survives_default_start_and_non_default_override},
        {"hash bounds and clear", test_hash_configuration_clamps_to_uci_bounds_and_clear_discards_warmed_entries},
        {"transposition table", test_transposition_table_stores_probes_and_clears_entries},
        {"transposition table mate normalization", test_transposition_table_preserves_mate_distance_across_plies},
    };

    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
