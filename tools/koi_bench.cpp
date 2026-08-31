#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"

namespace {

struct BenchmarkPosition {
    std::string_view name;
    std::string_view fen;
};

koi::SearchResult run_position(const BenchmarkPosition& benchmark) {
    const auto root = koi::GameState::from_fen(benchmark.fen);
    if (!root.has_value()) {
        throw std::runtime_error("invalid built-in benchmark position");
    }

    koi::SearchLimits limits;
    limits.depth = 3;
    std::optional<koi::SearchResult> result;
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    koi::SearchHandle handle = service.start(*root, limits, {.on_complete = [&result](const koi::SearchResult& completed) {
                                                result = completed;
                                            }});
    handle.wait();
    if (!result.has_value()) {
        throw std::runtime_error("benchmark search did not report a result");
    }
    return *result;
}

} // namespace

int main() {
    constexpr BenchmarkPosition kPositions[] = {
        {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
        {"tactics", "r1bq1rk1/pp3ppp/2n1pn2/2bp4/2B5/2NP1NP1/PPQ1PPBP/R3K2R w KQ - 4 8"},
    };

    try {
        std::cout << "Koi benchmark\n";
        for (const BenchmarkPosition& benchmark : kPositions) {
            const koi::SearchResult result = run_position(benchmark);
            std::cout << "position " << benchmark.name
                      << " depth " << result.completed_depth
                      << " nodes " << result.stats.nodes
                      << " qnodes " << result.stats.qnodes
                      << " tt_hits " << result.stats.tt_hits
                      << " score " << result.score_cp
                      << " move " << (result.best_move.has_value() ? result.best_move->uci() : "0000")
                      << '\n';
        }
    } catch (const std::exception&) {
        return 1;
    }
    return 0;
}
