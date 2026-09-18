#include <charconv>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/nnue.hpp"
#include "koi/search_service.hpp"
#include "koi/strength_suite.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.1.0"
#endif

namespace {

struct BenchmarkConfig {
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    bool timed = false;
    bool warm_hash = false;
    bool optional = false;
    std::optional<std::string> profile_json_path;
    std::optional<std::string> nnue_path;
};

constexpr std::string_view suite_name(const BenchmarkConfig& config) noexcept {
    return config.optional ? "optional_strength" : "strength";
}

struct BenchmarkRun {
    koi::SearchResult result;
    std::chrono::milliseconds wall_time{0};
    std::vector<koi::Move> pv;
};

bool accepts_move(const koi::StrengthPosition& benchmark, const koi::Move& move) {
    return std::find(benchmark.accepted_moves.begin(), benchmark.accepted_moves.end(), move.uci()) !=
        benchmark.accepted_moves.end();
}

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
        if (argument == "--optional") {
            config.optional = true;
            continue;
        }
        if (argument == "--profile-json") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            config.profile_json_path = argv[++index];
            continue;
        }
        if (argument == "--nnue") {
            // Optional trained network for the benchmark suite; the classical
            // evaluator is used when the flag is absent.
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            config.nnue_path = argv[++index];
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
           << "  \"build\": \"Koi Engine " KOI_ENGINE_BUILD_VERSION "\",\n"
           << "  \"suite\": \"" << suite_name(config) << "\",\n"
           << "  \"warm_hash\": " << (config.warm_hash ? "true" : "false") << ",\n"
           << "  \"hash_state\": \"" << (config.warm_hash ? "warm" : "cold") << "\",\n"
           << "  \"timed\": " << (config.timed ? "true" : "false") << ",\n"
           << "  \"hash_mb\": 512,\n"
           << "  \"threads\": " << config.threads << ",\n"
           << "  \"speed\": " << static_cast<unsigned>(config.speed_percent) << ",\n"
           << "  \"evaluator\": \"" << (config.nnue_path ? "nnue" : "classical") << "\",\n";
    if (config.nnue_path) {
        std::error_code file_error;
        const auto network_bytes = std::filesystem::file_size(*config.nnue_path, file_error);
        output << "  \"nnue\": {\"path\": ";
        write_json_string(output, *config.nnue_path);
        output << ", \"bytes\": " << (file_error ? 0 : network_bytes) << "},\n";
    }
    output << "  \"positions\": [\n";
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& [benchmark, run] = runs[index];
        const koi::SearchResult& result = run.result;
        const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
        const std::uint64_t elapsed = static_cast<std::uint64_t>(run.wall_time.count());
        const std::uint64_t nps = config.timed ?
            (elapsed > 0 ? visited * 1000 / elapsed : visited) : 0;
        output << "    {\"id\": ";
        write_json_string(output, benchmark.name);
        output << ", \"fen\": ";
        write_json_string(output, benchmark.fen);
        output << ", \"limits\": {\"depth\": " << static_cast<unsigned>(benchmark.depth)
               << "}, \"expected_move\": ";
        write_json_string(output, benchmark.expected_move);
        output << ", \"accepted_moves\": [";
        bool first_accepted_move = true;
        for (const std::string_view accepted_move : benchmark.accepted_moves) {
            if (accepted_move.empty()) {
                continue;
            }
            if (!first_accepted_move) {
                output << ", ";
            }
            write_json_string(output, accepted_move);
            first_accepted_move = false;
        }
        output << "], \"category\": ";
        write_json_string(output, benchmark.category);
        output << ", \"hash_mb\": 512, \"hash_state\": \""
               << (config.warm_hash ? "warm" : "cold") << "\", \"threads\": " << config.threads
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
                << "}, \"nps\": " << nps;
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
        std::cerr << "usage: koi-bench [--threads N] [--speed 1-100] [--timed] [--warm-hash] [--optional] [--nnue path] [--profile-json path]\n";
        return 2;
    }

    try {
        std::cout << "Koi benchmark\n";
        std::cout << "config threads " << config->threads
                  << " speed " << static_cast<unsigned>(config->speed_percent)
                  << " timed " << (config->timed ? 1 : 0)
                  << " hash " << (config->warm_hash ? "warm" : "cold");
        if (config->optional) {
            std::cout << " suite " << suite_name(*config);
        }
        std::cout << '\n';
        const std::span<const koi::StrengthPosition> benchmarks = config->optional ?
            koi::optional_strength_positions() : koi::strength_positions();
        std::vector<std::pair<koi::StrengthPosition, BenchmarkRun>> profile_runs;
        if (config->profile_json_path.has_value()) {
            profile_runs.reserve(benchmarks.size());
        }
        koi::EvaluatorSelection selection;
        if (config->nnue_path.has_value()) {
            selection = koi::make_evaluator(std::filesystem::path{*config->nnue_path});
            if (selection.nnue_error.has_value()) {
                std::cerr << "koi-bench: NNUE network rejected ("
                          << selection.nnue_error->message
                          << "); using the classical evaluator.\n";
            }
        }
        if (!selection.evaluator) {
            selection.evaluator = std::make_shared<koi::ClassicalEvaluator>();
        }
        koi::SearchService service(std::move(selection.evaluator));
        for (const koi::StrengthPosition& benchmark : benchmarks) {
            if (!config->warm_hash) {
                // Reuse the 512 MB allocation while keeping every cold position
                // independent of entries produced by its predecessor.
                service.clear_hash();
            }
            const BenchmarkRun run = run_position(benchmark, *config, service);
            const koi::SearchResult& result = run.result;
            const bool matched = result.best_move.has_value() && accepts_move(benchmark, *result.best_move);
            std::cout << "position " << benchmark.name
                      << " depth " << result.completed_depth
                      << " nodes " << result.stats.nodes
                      << " qnodes " << result.stats.qnodes
                      << " tt_hits " << result.stats.tt_hits
                      << " score " << result.score_cp
                      << " expected " << benchmark.expected_move
                      << " move " << (result.best_move.has_value() ? result.best_move->uci() : "0000")
                      << " match " << (matched ? 1 : 0);
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
