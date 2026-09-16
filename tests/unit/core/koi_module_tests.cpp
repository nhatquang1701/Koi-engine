import koi;

#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

} // namespace

int main() {
    try {
        require(koi::module_api::architecture_version == 1,
                "module architecture version must be stable");
        koi::module_api::PositionContract position{};
        position.zobrist_key = 0x1234;
        position.piece_count = 32;
        require(position.zobrist_key == 0x1234 && position.piece_count == 32,
                "position contract must be value-oriented");

        koi::module_api::SearchSessionContract session{};
        session.thread_count = 1;
        require(!session.cancelled && session.thread_count == 1,
                "search session contract must expose cancellation and thread state");
        koi::module_api::SearchLimitsContract limits{};
        limits.depth = 12;
        limits.node_budget = 50'000;
        require(limits.depth == 12 && limits.node_budget == 50'000,
                "search limits contract must carry deterministic limits");
        koi::module_api::SearchWorkerContextContract worker{};
        worker.worker_id = 2;
        worker.nodes = 128;
        require(worker.worker_id == 2 && worker.nodes == 128,
                "worker context contract must expose local counters");
        koi::module_api::RootCoordinatorContract coordinator{};
        coordinator.root_move_count = 20;
        coordinator.deterministic_tiebreak = true;
        require(coordinator.root_move_count == 20 && coordinator.deterministic_tiebreak,
                "root coordinator contract must expose stable ordering");
        koi::module_api::SearchResultContract result{};
        result.score_cp = 34;
        result.completed_depth = 8;
        require(result.score_cp == 34 && result.completed_depth == 8,
                "search result contract must expose completed iteration data");
        require(koi::module_api::aggregate_boundary_version == 1,
                "aggregate module boundary version must be stable");
        std::cout << "PASS module contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL module contracts: " << error.what() << '\n';
        return 1;
    }
}
