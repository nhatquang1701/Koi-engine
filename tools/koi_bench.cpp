#include <charconv>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/strength_suite.hpp"

namespace {

struct BenchmarkConfig {
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    bool timed = false;
    bool warm_hash = false;
    std::optional<std::string> profile_json_path;
};

struct BenchmarkRun {
    koi::SearchResult result;
    std::chrono::milliseconds wall_time{0};
    std::vector<koi::Move> pv;
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
        if (argument == "--warm-hash") {
            config.warm_hash = true;
            continue;
        }
        if (argument == "--profile-json") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            config.profile_json_path = argv[++index];
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

BenchmarkRun run_position(const koi::StrengthPosition& benchmark, const BenchmarkConfig& config,
                          koi::SearchService& service) {
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
    std::vector<koi::Move> pv;
    const auto started = std::chrono::steady_clock::now();
    koi::SearchHandle handle = service.start(*root, limits,
        {.on_info = [&pv](const koi::SearchInfo& info) {
             if (info.multipv == 1 && !info.pv.empty()) {
                 pv = info.pv;
             }
         },
         .on_complete = [&result](const koi::SearchResult& completed) {
             result = completed;
         }}, options);
    handle.wait();
    if (!result.has_value()) {
        throw std::runtime_error("benchmark search did not report a result");
    }
    return {*result, std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started), std::move(pv)};
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const char character : value) {
        switch (character) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default: output << character; break;
        }
    }
    output << '"';
}

void write_profile_json(const std::string& path, const BenchmarkConfig& config,
                        const std::vector<std::pair<koi::StrengthPosition, BenchmarkRun>>& runs) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to write benchmark profile JSON");
    }

    output << "{\n  \"schema\": \"koi-bench-profile-v1\",\n"
           << "  \"engine\": \"Koi Engine\",\n"
           << "  \"build\": \"unknown\",\n"
           << "  \"suite\": \"strength\",\n"
           << "  \"warm_hash\": " << (config.warm_hash ? "true" : "false") << ",\n"
           << "  \"hash_mb\": 16,\n"
           << "  \"threads\": " << config.threads << ",\n"
           << "  \"speed\": " << static_cast<unsigned>(config.speed_percent) << ",\n"
           << "  \"positions\": [\n";
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& [benchmark, run] = runs[index];
        const koi::SearchResult& result = run.result;
        const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
        const std::uint64_t elapsed = static_cast<std::uint64_t>(
            (config.timed ? run.wall_time : result.stats.elapsed).count());
        output << "    {\"id\": ";
        write_json_string(output, benchmark.name);
        output << ", \"fen\": ";
        write_json_string(output, benchmark.fen);
        output << ", \"limits\": {\"depth\": " << static_cast<unsigned>(benchmark.depth)
               << "}, \"hash_mb\": 16, \"threads\": " << config.threads
               << ", \"speed\": " << static_cast<unsigned>(config.speed_percent)
               << ", \"score_cp\": " << result.score_cp << ", \"pv\": [";
        if (!run.pv.empty()) {
            for (std::size_t pv_index = 0; pv_index < run.pv.size(); ++pv_index) {
                if (pv_index != 0) {
                    output << ", ";
                }
                write_json_string(output, run.pv[pv_index].uci());
            }
        } else if (result.best_move.has_value()) {
            write_json_string(output, result.best_move->uci());
        }
        output << "], \"nodes\": " << result.stats.nodes
               << ", \"qnodes\": " << result.stats.qnodes
               << ", \"tt_hits\": " << result.stats.tt_hits
               << ", \"pruning\": {\"pvs_searches\": " << result.stats.pvs_searches
               << ", \"pvs_researches\": " << result.stats.pvs_researches
               << ", \"aspiration_researches\": " << result.stats.aspiration_researches
               << ", \"check_extensions\": " << result.stats.check_extensions
               << ", \"qchecks\": " << result.stats.qchecks
               << ", \"see_prunes\": " << result.stats.see_prunes
               << ", \"delta_prunes\": " << result.stats.delta_prunes
               << ", \"null_cutoffs\": " << result.stats.null_cutoffs
               << ", \"lmr_reductions\": " << result.stats.lmr_reductions
               << "}, \"nps\": " << (elapsed > 0 ? visited * 1000 / elapsed : visited);
        if (config.timed) {
            output << ", \"elapsed_ms\": " << run.wall_time.count();
        }
        output << '}' << (index + 1 == runs.size() ? '\n' : ',') << '\n';
    }
    output << "  ]\n}\n";
    if (!output) {
        throw std::runtime_error("failed while writing benchmark profile JSON");
    }
}

} // namespace

int main(int argc, char** argv) {
    const auto config = parse_arguments(argc, argv);
    if (!config.has_value()) {
        std::cerr << "usage: koi-bench [--threads N] [--speed 1-100] [--timed] [--warm-hash] [--profile-json path]\n";
        return 2;
    }

    try {
        std::cout << "Koi benchmark\n";
        if (config->threads != 1 || config->speed_percent != 100 || config->timed || config->warm_hash) {
            std::cout << "config threads " << config->threads
                      << " speed " << static_cast<unsigned>(config->speed_percent)
                      << " timed " << (config->timed ? 1 : 0);
            if (config->warm_hash) {
                std::cout << " warm_hash 1";
            }
            std::cout << '\n';
        }
        std::vector<std::pair<koi::StrengthPosition, BenchmarkRun>> profile_runs;
        if (config->profile_json_path.has_value()) {
            profile_runs.reserve(koi::strength_positions().size());
        }
        std::optional<koi::SearchService> warm_service;
        if (config->warm_hash) {
            warm_service.emplace(std::make_shared<koi::ClassicalEvaluator>());
        }
        for (const koi::StrengthPosition& benchmark : koi::strength_positions()) {
            koi::SearchService cold_service(std::make_shared<koi::ClassicalEvaluator>());
            koi::SearchService& service = warm_service.has_value() ? *warm_service : cold_service;
            const BenchmarkRun run = run_position(benchmark, *config, service);
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
            if (config->profile_json_path.has_value()) {
                profile_runs.emplace_back(benchmark, run);
            }
        }
        if (config->profile_json_path.has_value()) {
            write_profile_json(*config->profile_json_path, *config, profile_runs);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
