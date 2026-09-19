// Focused unit coverage for the striped transposition table: entry round trips,
// mate-score normalisation, replacement generational ordering, Clear Hash and
// resizing.  These behaviours are otherwise only reachable through a full
// search, where failures are hard to localise.

#include <cstdint>
#include <thread>
#include <vector>

#include "koi/move.hpp"
#include "koi/transposition_table.hpp"
#include "koi_test_support.hpp"

namespace {

using koi::Move;
using koi::TranspositionBound;
using koi::TranspositionTable;
using koi::test::require;

[[nodiscard]] Move parse_move(std::string_view text) {
    return koi::test::require_value(Move::parse_uci(text), "test move must parse");
}

void test_store_and_probe_round_trip() {
    TranspositionTable table(1);
    const Move move = parse_move("e2e4");

    table.store(0x1234ULL, 7, 42, TranspositionBound::exact, move, 0, true);

    const auto entry =
        koi::test::require_value(table.probe(0x1234ULL), "a stored entry must probe");
    require(entry.key == 0x1234ULL, "probe must return the stored key");
    require(entry.depth == 7, "probe must return the stored depth");
    require(entry.score == 42, "probe must return the stored score");
    require(entry.bound == TranspositionBound::exact, "probe must return the stored bound");
    require(entry.best_move.uci() == "e2e4", "probe must return the stored move");
    require(entry.occupied, "a probed entry must be occupied");
    require(entry.pv, "probe must return the stored PV flag");
    require(entry.generation_age == 0, "an entry stored in the current epoch must have age zero");
    require(!table.probe(0x9999ULL).has_value(), "unknown keys must not probe");
}

void test_mate_scores_are_normalised_by_ply() {
    TranspositionTable table(1);
    const Move move = parse_move("a1a2");

    // Mate scores are stored relative to the root and restored at probe time, so
    // a probe at the storing ply must return exactly the stored score.
    table.store(11, 5, 99997, TranspositionBound::exact, move, 2);
    require(koi::test::require_value(table.probe(11, 2), "the mate entry must probe").score == 99997,
            "a mate score must round trip at its own ply");
    require(koi::test::require_value(table.probe(11, 0), "the mate entry must probe").score == 99999,
            "the root-relative mate score must grow with the storing ply");

    table.store(12, 5, -99997, TranspositionBound::exact, move, 3);
    require(koi::test::require_value(table.probe(12, 3), "the losing mate entry must probe").score ==
                -99997,
            "a negative mate score must round trip at its own ply");
    require(koi::test::require_value(table.probe(12, 0), "the losing mate entry must probe").score ==
                -100000,
            "the root-relative losing mate score must move away from zero");
}

void test_deeper_entries_survive_shallower_stores() {
    TranspositionTable table(1);
    const Move move = parse_move("e2e4");

    table.store(7, 3, 10, TranspositionBound::exact, move);
    table.store(7, 5, 20, TranspositionBound::exact, move);
    require(koi::test::require_value(table.probe(7), "the deeper entry must probe").depth == 5,
            "a deeper store must replace a shallower entry");

    table.store(7, 4, 30, TranspositionBound::exact, move);
    const auto entry = koi::test::require_value(table.probe(7), "the deeper entry must survive");
    require(entry.depth == 5 && entry.score == 20,
            "a shallower store must not displace a deeper exact entry");
}

void test_new_generation_ages_entries() {
    TranspositionTable table(1);
    const Move move = parse_move("e2e4");

    table.store(21, 4, 1, TranspositionBound::lower, move);
    require(koi::test::require_value(table.probe(21), "the entry must probe").generation_age == 0,
            "an entry stored this epoch must start at age zero");

    table.new_generation();
    require(koi::test::require_value(table.probe(21), "the entry must survive a new epoch")
                .generation_age == 1,
            "a new search epoch must age the entry by one");

    table.new_generation();
    require(koi::test::require_value(table.probe(21), "the entry must survive two epochs")
                .generation_age == 2,
            "each search epoch must age the entry further");
}

void test_clear_invalidates_entries_and_resets_hashfull() {
    TranspositionTable table(1);
    const Move move = parse_move("e2e4");

    // Writing far more keys than the one-megabyte table has clusters guarantees
    // every sampled slot holds a current-epoch entry.
    for (std::uint64_t key = 0; key < 65536; ++key) {
        table.store(key, 1, 0, TranspositionBound::exact, move);
    }
    require(table.hashfull_permill() == 1000,
            "a fully populated table must report full hashfull occupancy");

    table.clear();
    require(table.hashfull_permill() == 0, "Clear Hash must reset the reported occupancy");
    require(!table.probe(0).has_value(), "Clear Hash must invalidate stored entries");
}

void test_resize_changes_the_reported_size() {
    TranspositionTable table(1);
    require(table.size_mb() == 1, "the table must start at the requested size");

    (void)table.set_size_mb(2);
    require(table.size_mb() == 2, "a larger request must be applied");

    (void)table.set_size_mb(0);
    require(table.size_mb() == 1, "requests below the minimum must clamp to one megabyte");
}

void test_power_of_two_rounding_and_prefetch() {
    // Cluster indexing masks the key, so the allocation rounds down to the
    // nearest power of two clusters and the reported size follows it.
    TranspositionTable table(3);
    require(table.size_mb() == 2, "a three megabyte request must round down to two");

    (void)table.set_size_mb(5);
    require(table.size_mb() == 4, "a five megabyte request must round down to four");

    (void)table.set_size_mb(7);
    require(table.size_mb() == 4, "a seven megabyte request must round down to four");

    // A prefetch is advisory: it must be safe before a probe and must not
    // change what the probe finds.
    const Move move = parse_move("e2e4");
    table.prefetch(0xABCDULL);
    table.store(0xABCDULL, 6, 12, TranspositionBound::exact, move);
    table.prefetch(0xABCDULL);
    require(koi::test::require_value(table.probe(0xABCDULL),
                                     "prefetch must not disturb a probe")
                .score == 12,
            "a prefetched probe must return the stored entry");
}

void test_cluster_masking_covers_every_slot() {
    TranspositionTable table(2);
    const Move move = parse_move("e2e4");

    // Fewer keys than clusters, so every stored key must survive without a
    // replacement collision; this catches a mask/segment-address mistake.
    for (std::uint64_t key = 0; key < 4096; ++key) {
        table.store(key, 2, static_cast<int>(key), TranspositionBound::exact, move);
    }
    for (std::uint64_t key = 0; key < 4096; ++key) {
        require(table.probe(key).has_value(), "every stored key must probe");
    }
}

void test_concurrent_store_and_probe_is_safe() {
    TranspositionTable table(1);
    const Move move = parse_move("e2e4");
    constexpr std::uint64_t kThreads = 4;
    constexpr std::uint64_t kPerThread = 512;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::uint64_t thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&table, move, thread]() {
            const std::uint64_t base = 0x100000ULL * (thread + 1);
            for (std::uint64_t index = 0; index < kPerThread; ++index) {
                table.store(base + index, 2, static_cast<int>(index), TranspositionBound::lower,
                            move);
                (void)table.probe(base + index);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    for (std::uint64_t thread = 0; thread < kThreads; ++thread) {
        const std::uint64_t base = 0x100000ULL * (thread + 1);
        require(table.probe(base).has_value(), "concurrently stored entries must survive");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests = {
        {"transposition store and probe", test_store_and_probe_round_trip},
        {"transposition mate normalisation", test_mate_scores_are_normalised_by_ply},
        {"transposition replacement depth", test_deeper_entries_survive_shallower_stores},
        {"transposition generation ageing", test_new_generation_ages_entries},
        {"transposition clear and hashfull", test_clear_invalidates_entries_and_resets_hashfull},
        {"transposition resize bounds", test_resize_changes_the_reported_size},
        {"transposition power of two rounding", test_power_of_two_rounding_and_prefetch},
        {"transposition cluster masking", test_cluster_masking_covers_every_slot},
        {"transposition concurrent access", test_concurrent_store_and_probe_is_safe},
    };
    return koi::test::run_tests(tests, argc, argv);
}
