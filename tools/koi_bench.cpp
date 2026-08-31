#include <charconv>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/strength_suite.hpp"

namespace {

struct BenchmarkConfig {
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    bool timed = false;
};

struct BenchmarkRun {
    koi::SearchResult result;
    std::chrono::milliseconds wall_time{0};
};

bool parse_uint64(std::string_view value, std::uint64_t& parsed) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

std::optional<BenchmarkConfig> parse_arguments(int argc, char** argv) {
    BenchmarkConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--timed") {
            config.timed = true;
            continue;
        }
        if (argument != "--threads" && argument != "--speed") {
            return std::nullopt;
        }
        if (index + 1 >= argc) {
            return std::nullopt;
        }

        std::uint64_t value = 0;
        if (!parse_uint64(argv[++index], value)) {
            return std::nullopt;
        }
        if (argument == "--threads") {
            if (value == 0) {
                return std::nullopt;
            }
            config.threads = std::min<std::size_t>(static_cast<std::size_t>(value),
                                                   koi::maximum_search_threads());
        } else if (value < 1 || value > 100) {
            return std::nullopt;
        } else {
            config.speed_percent = static_cast<std::uint8_t>(value);
        }
    }
    return config;
}

BenchmarkRun run_position(const koi::StrengthPosition& benchmark, const BenchmarkConfig& config) {
    const auto root = koi::GameState::from_fen(benchmark.fen);
    if (!root.has_value()) {
        throw std::runtime_error("invalid built-in benchmark position");
    }

    koi::SearchLimits limits;
    limits.depth = benchmark.depth;
    koi::SearchOptions options;
    options.threads = config.threads;
    options.speed_percent = config.speed_percent;

    std::optional<koi::SearchResult> result;
    koi::SearchService service(std::make_shared<koi::ClassicalEvaluator>());
    const auto started = std::chrono::steady_clock::now();
    koi::SearchHandle handle = service.start(*root, limits,
        {.on_complete = [&result](const koi::SearchResult& completed) {
            result = completed;
        }}, options);
    handle.wait();
    if (!result.has_value()) {
        throw std::runtime_error("benchmark search did not report a result");
    }
    return {*result, std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started)};
}

} // namespace

int main(int argc, char** argv) {
    const auto config = parse_arguments(argc, argv);
    if (!config.has_value()) {
        std::cerr << "usage: koi-bench [--threads N] [--speed 1-100] [--timed]\n";
        return 2;
    }

    try {
        std::cout << "Koi benchmark\n";
        if (config->threads != 1 || config->speed_percent != 100 || config->timed) {
            std::cout << "config threads " << config->threads
                      << " speed " << static_cast<unsigned>(config->speed_percent)
                      << " timed " << (config->timed ? 1 : 0) << '\n';
        }
        for (const koi::StrengthPosition& benchmark : koi::strength_positions()) {
            const BenchmarkRun run = run_position(benchmark, *config);
            const koi::SearchResult& result = run.result;
            std::cout << "position " << benchmark.name
                      << " depth " << result.completed_depth
                      << " nodes " << result.stats.nodes
                      << " qnodes " << result.stats.qnodes
                      << " tt_hits " << result.stats.tt_hits
                      << " score " << result.score_cp
                      << " expected " << benchmark.expected_move
                      << " move " << (result.best_move.has_value() ? result.best_move->uci() : "0000")
                      << " match " << (result.best_move.has_value() &&
                                          result.best_move->uci() == benchmark.expected_move ? 1 : 0);
            if (config->timed) {
                const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
                const std::uint64_t elapsed = static_cast<std::uint64_t>(run.wall_time.count());
                std::cout << " elapsed_ms " << run.wall_time.count()
                          << " nps " << (elapsed > 0 ? visited * 1000 / elapsed : visited);
            }
            std::cout << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
