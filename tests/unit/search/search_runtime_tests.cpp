#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "koi/detail/search_budget.hpp"
#include "koi/detail/search_table_access.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::Move require_move(std::string_view uci) {
    const auto parsed = koi::Move::parse_uci(uci);
    require(parsed.has_value(), "test move must parse");
    return *parsed;
}

koi::TimeManager node_manager(std::uint64_t limit) {
    koi::SearchLimits limits;
    limits.nodes = limit;
    return koi::TimeManager(limits, koi::Color::white);
}

void test_enabled_table_access_delegates_probe_and_store() {
    koi::TranspositionTable table(1);
    koi::detail::SearchTableAccess access(table);
    const koi::Move move = require_move("e2e4");
    constexpr std::uint64_t key = 0x0123456789abcdefULL;

    require(access.enabled(), "table access must be enabled by default");
    access.store(key, 6, 17, koi::TranspositionBound::exact, move);
    const auto entry = access.probe(key);
    require(entry.has_value(), "enabled table access must expose stored entries");
    require(entry->depth == 6 && entry->score == 17 &&
                entry->bound == koi::TranspositionBound::exact && entry->best_move == move,
            "enabled table access must preserve TT entry values");
}

void test_disabled_table_access_is_a_noop_and_does_not_hide_existing_storage() {
    koi::TranspositionTable table(1);
    koi::detail::SearchTableAccess access(table);
    const koi::Move move = require_move("e2e4");
    constexpr std::uint64_t existing_key = 0x1111ULL;
    constexpr std::uint64_t disabled_key = 0x2222ULL;

    access.store(existing_key, 4, 11, koi::TranspositionBound::exact, move);
    access.set_enabled(false);
    require(!access.enabled(), "table access must report its disabled state");
    require(!access.probe(existing_key).has_value(),
            "disabled table access must not expose TT entries");
    access.store(disabled_key, 4, 22, koi::TranspositionBound::exact, move);

    access.set_enabled(true);
    require(access.enabled(), "table access must be re-enableable");
    require(access.probe(existing_key).has_value(),
            "disabling an access view must not erase shared TT storage");
    require(!access.probe(disabled_key).has_value(),
            "disabled table access must not write to shared TT storage");
}

void test_local_budget_rejects_the_first_node_at_the_limit() {
    auto manager = node_manager(3);
    koi::detail::SearchBudget budget(manager);

    require(!budget.uses_shared_counter(), "local budget must not report shared accounting");
    require(budget.reserve(0), "local budget must reserve its first node");
    require(budget.reserve(1), "local budget must reserve its second node");
    require(budget.reserve(2), "local budget must reserve its final node");
    require(!budget.reserve(3), "local budget must reject a reservation at the hard limit");
    require(budget.visited(3) == 3, "local budget must report the caller's local node count");
}

void test_shared_budget_reservation_is_capped_by_the_global_limit() {
    auto manager = node_manager(3);
    std::atomic<std::uint64_t> shared_nodes = 2;
    koi::detail::SearchBudget budget(manager, &shared_nodes);

    require(budget.uses_shared_counter(), "shared budget must report shared accounting");
    require(budget.reserve(100), "shared budget must reserve the final available node");
    require(shared_nodes.load(std::memory_order_relaxed) == 3,
            "shared budget must increment the shared counter exactly once");
    require(!budget.reserve(100), "shared budget must reject reservations at the hard limit");
    require(shared_nodes.load(std::memory_order_relaxed) == 3,
            "shared budget must never overshoot the hard limit");
    require(budget.visited(0) == 3,
            "shared budget must report the shared counter rather than local nodes");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const TestCase tests[] = {
        {"enabled table access", test_enabled_table_access_delegates_probe_and_store},
        {"disabled table access", test_disabled_table_access_is_a_noop_and_does_not_hide_existing_storage},
        {"local node budget", test_local_budget_rejects_the_first_node_at_the_limit},
        {"shared node budget", test_shared_budget_reservation_is_capped_by_the_global_limit},
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
