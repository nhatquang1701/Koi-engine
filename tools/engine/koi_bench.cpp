#include <charconv>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/nnue.hpp"
#include "koi/search_service.hpp"
#include "koi/strength_suite.hpp"

#ifndef KOI_ENGINE_BUILD_VERSION
#define KOI_ENGINE_BUILD_VERSION "1.0.0"
#endif

namespace {

struct BenchmarkConfig {
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    bool timed = false;
    bool warm_hash = false;
    bool optional = false;
    std::uint64_t node_limit = 0;
    std::size_t warmup = 0;
    std::size_t repeat = 1;
    std::uint8_t default_fen_depth = 6;
    std::optional<std::string> fen_file_path;
    std::optional<std::uint8_t> fixed_depth;
    std::optional<std::pair<std::uint8_t, std::uint8_t>> depth_sweep;
    std::optional<std::string> profile_json_path;
    std::optional<std::string> report_json_path;
    std::optional<std::string> nnue_path;
};

std::string suite_name(const BenchmarkConfig& config) {
    if (config.fen_file_path.has_value()) {
        return "fen_file";
    }
    return config.optional ? "optional_strength" : "strength";
}

// One measurement position: either a built-in strength fixture or one FEN
// line from an external list. The depth is resolved before the run starts.
struct BenchPosition {
    std::string name;
    std::string fen;
    std::string expected_move;
    std::array<std::string, 3> accepted_moves{};
    std::string category;
    std::uint8_t depth = 0;
};

// One row of a time-to-depth table: the state of an iterative-deepening
// iteration as reported by the search info callback.
struct DepthSample {
    std::uint8_t depth = 0;
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    std::uint64_t elapsed_ms = 0;
    std::uint64_t nps = 0;
    int score_cp = 0;
    std::string best_move;
};

struct BenchmarkRun {
    koi::SearchResult result;
    std::chrono::milliseconds wall_time{0};
    std::vector<koi::Move> pv;
    std::vector<DepthSample> samples;
};

struct PositionProfile {
    BenchPosition benchmark;
    std::uint8_t measured_depth = 0;
    std::vector<BenchmarkRun> runs;
};

bool accepts_move(const BenchPosition& benchmark, const koi::Move& move) {
    return std::find(benchmark.accepted_moves.begin(), benchmark.accepted_moves.end(), move.uci()) !=
        benchmark.accepted_moves.end();
}

bool parse_uint64(std::string_view value, std::uint64_t& parsed) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    return error == std::errc{} && end == value.data() + value.size();
}

bool parse_depth_sweep(std::string_view value, std::uint8_t& from, std::uint8_t& to) {
    const std::size_t separator = value.find("..");
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 2 >= value.size()) {
        return false;
    }
    std::uint64_t low = 0;
    std::uint64_t high = 0;
    if (!parse_uint64(value.substr(0, separator), low) ||
        !parse_uint64(value.substr(separator + 2), high)) {
        return false;
    }
    if (low < 1 || high > 255 || low > high) {
        return false;
    }
    from = static_cast<std::uint8_t>(low);
    to = static_cast<std::uint8_t>(high);
    return true;
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
        if (argument == "--report") {
            // Speed artifact for tools/build/speed_gate.ps1. The schema is
            // koi-bench-speed-v1 and is independent of the text report.
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            config.report_json_path = argv[++index];
            continue;
        }
        if (argument == "--fen-file") {
            // External position list: one FEN per line, blank lines and
            // #-prefixed comments are ignored.
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            config.fen_file_path = argv[++index];
            continue;
        }
        if (argument == "--depth") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            if (!parse_uint64(argv[++index], value) || value < 1 || value > 255) {
                return std::nullopt;
            }
            config.fixed_depth = static_cast<std::uint8_t>(value);
            continue;
        }
        if (argument == "--depth-sweep") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            std::uint8_t from = 0;
            std::uint8_t to = 0;
            if (!parse_depth_sweep(argv[++index], from, to)) {
                return std::nullopt;
            }
            config.depth_sweep = std::pair<std::uint8_t, std::uint8_t>{from, to};
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
        if (argument == "--nodes") {
            // Node-limited mode: measures the steady-state search instead of a
            // fixed shallow depth, matching the SPRT/node-limited match regime.
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            if (!parse_uint64(argv[++index], value) || value == 0) {
                return std::nullopt;
            }
            config.node_limit = value;
            continue;
        }
        if (argument == "--warmup" || argument == "--repeat") {
            if (index + 1 >= argc) {
                return std::nullopt;
            }
            std::uint64_t value = 0;
            if (!parse_uint64(argv[++index], value)) {
                return std::nullopt;
            }
            if (argument == "--warmup") {
                config.warmup = static_cast<std::size_t>(value);
            } else {
                if (value == 0) {
                    return std::nullopt;
                }
                config.repeat = static_cast<std::size_t>(value);
            }
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
    if (config.fixed_depth.has_value() && config.depth_sweep.has_value()) {
        return std::nullopt;
    }
    if (config.depth_sweep.has_value() && config.node_limit != 0) {
        return std::nullopt;
    }
    return config;
}

std::vector<BenchPosition> load_suite_positions(const bool optional) {
    const std::span<const koi::StrengthPosition> suite =
        optional ? koi::optional_strength_positions() : koi::strength_positions();
    std::vector<BenchPosition> positions;
    positions.reserve(suite.size());
    for (const koi::StrengthPosition& fixture : suite) {
        BenchPosition position;
        position.name = std::string(fixture.name);
        position.fen = std::string(fixture.fen);
        position.expected_move = std::string(fixture.expected_move);
        for (std::size_t index = 0; index < position.accepted_moves.size(); ++index) {
            position.accepted_moves[index] = std::string(fixture.accepted_moves[index]);
        }
        position.category = std::string(fixture.category);
        position.depth = fixture.depth;
        positions.push_back(std::move(position));
    }
    return positions;
}

std::string_view trim(std::string_view value) {
    const auto is_space = [](const char character) {
        return std::isspace(static_cast<unsigned char>(character)) != 0;
    };
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<BenchPosition> load_fen_positions(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("unable to open FEN file: " + path);
    }
    std::vector<BenchPosition> positions;
    std::string line;
    while (std::getline(input, line)) {
        const std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        // A line is `[name|]<FEN or UCI move list>`. The move-list form keeps
        // opening corpora (name | e2e4 e7e5 ...) usable for depth sweeps.
        std::string_view name;
        std::string_view payload = trimmed;
        const std::size_t separator = trimmed.find('|');
        if (separator != std::string_view::npos) {
            name = trim(trimmed.substr(0, separator));
            payload = trim(trimmed.substr(separator + 1));
        }
        if (payload.empty()) {
            throw std::runtime_error("empty benchmark position in FEN file: " + path);
        }
        BenchPosition position;
        if (payload.find('/') != std::string_view::npos) {
            const auto state = koi::GameState::from_fen(payload);
            if (!state.has_value()) {
                throw std::runtime_error("invalid benchmark position: " + std::string(payload));
            }
            position.fen = state->fen();
        } else {
            koi::GameState state = koi::GameState::startpos();
            std::size_t cursor = 0;
            while (cursor < payload.size()) {
                while (cursor < payload.size() && std::isspace(static_cast<unsigned char>(payload[cursor])) != 0) {
                    ++cursor;
                }
                const std::size_t token_start = cursor;
                while (cursor < payload.size() && std::isspace(static_cast<unsigned char>(payload[cursor])) == 0) {
                    ++cursor;
                }
                if (token_start == cursor) {
                    break;
                }
                const std::string_view token = payload.substr(token_start, cursor - token_start);
                const auto move = koi::Move::parse_uci(token);
                if (!move.has_value() || !state.make_move(*move)) {
                    throw std::runtime_error("illegal UCI move in FEN file: " + std::string(token));
                }
            }
            position.fen = state.fen();
        }
        position.category = "fen-file";
        if (!name.empty()) {
            position.name = std::string(name);
        } else {
            const std::size_t index = positions.size() + 1;
            position.name = "fen-";
            if (index < 100) {
                position.name += '0';
            }
            if (index < 10) {
                position.name += '0';
            }
            position.name += std::to_string(index);
        }
        positions.push_back(std::move(position));
    }
    if (positions.empty()) {
        throw std::runtime_error("FEN file contains no positions: " + path);
    }
    return positions;
}

std::uint64_t compute_nps(const std::uint64_t visited, const std::uint64_t elapsed_ms) {
    return elapsed_ms > 0 ? visited * 1000 / elapsed_ms : visited;
}

DepthSample final_sample(const koi::SearchResult& result, const std::uint64_t elapsed_ms) {
    DepthSample sample;
    sample.depth = static_cast<std::uint8_t>(std::clamp(result.completed_depth, 0, 255));
    sample.nodes = result.stats.nodes;
    sample.qnodes = result.stats.qnodes;
    sample.tt_hits = result.stats.tt_hits;
    sample.elapsed_ms = elapsed_ms;
    sample.nps = compute_nps(sample.nodes + sample.qnodes, elapsed_ms);
    sample.score_cp = result.score_cp;
    if (result.best_move.has_value()) {
        sample.best_move = result.best_move->uci();
    }
    return sample;
}

BenchmarkRun run_position(const BenchPosition& benchmark, const BenchmarkConfig& config,
                          koi::SearchService& service) {
    const auto root = koi::GameState::from_fen(benchmark.fen);
    if (!root.has_value()) {
        throw std::runtime_error("invalid benchmark position: " + benchmark.name);
    }

    koi::SearchLimits limits;
    if (config.node_limit != 0) {
        limits.nodes = config.node_limit;
    } else {
        limits.depth = static_cast<int>(benchmark.depth);
    }
    koi::SearchOptions options;
    options.threads = config.threads;
    options.speed_percent = config.speed_percent;

    std::optional<koi::SearchResult> result;
    std::vector<koi::Move> pv;
    std::vector<DepthSample> samples;
    // In sweep mode every iterative-deepening info line is retained so the
    // report carries the full time-to-depth table; other modes only keep the
    // completed search.
    const bool sweep = config.depth_sweep.has_value();
    const auto started = std::chrono::steady_clock::now();
    koi::SearchHandle handle = service.start(*root, limits,
        {.on_info = [&pv, &samples, sweep, &config](const koi::SearchInfo& info) {
             if (info.multipv != 1 || info.pv.empty()) {
                 return;
             }
             pv = info.pv;
             if (!sweep || info.depth <= 0) {
                 return;
             }
             DepthSample sample;
             sample.depth = static_cast<std::uint8_t>(std::min(info.depth, 255));
             if (sample.depth < config.depth_sweep->first ||
                 sample.depth > config.depth_sweep->second) {
                 return;
             }
             // SearchInfo::nodes counts every visited node, quiescence
             // included, and SearchInfo::qnodes repeats the quiescence
             // subset; split them so sweep samples match the final sample
             // and the totals, which come from SearchStats.
             const std::uint64_t info_qnodes = info.qnodes;
             sample.nodes = info.nodes >= info_qnodes ? info.nodes - info_qnodes : 0;
             sample.qnodes = info_qnodes;
             sample.tt_hits = info.tt_hits;
             sample.elapsed_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, info.elapsed.count()));
             sample.nps = compute_nps(sample.nodes + sample.qnodes, sample.elapsed_ms);
             sample.score_cp = info.score_cp;
             sample.best_move = info.pv.front().uci();
             const auto existing = std::find_if(samples.begin(), samples.end(),
                 [&sample](const DepthSample& candidate) { return candidate.depth == sample.depth; });
             if (existing == samples.end()) {
                 samples.push_back(std::move(sample));
             } else {
                 *existing = std::move(sample);
             }
         },
         .on_complete = [&result](const koi::SearchResult& completed) {
             result = completed;
         }}, options);
    handle.wait();
    if (!result.has_value()) {
        throw std::runtime_error("benchmark search did not report a result");
    }
    const auto wall_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    if (!sweep) {
        samples.clear();
    }
    const DepthSample completed = final_sample(*result, static_cast<std::uint64_t>(wall_time.count()));
    const auto existing = std::find_if(samples.begin(), samples.end(),
        [&completed](const DepthSample& candidate) { return candidate.depth == completed.depth; });
    if (existing == samples.end()) {
        samples.push_back(completed);
    } else {
        *existing = completed;
    }
    std::sort(samples.begin(), samples.end(), [](const DepthSample& left, const DepthSample& right) {
        return left.depth < right.depth;
    });
    return {*result, wall_time, std::move(pv), std::move(samples)};
}

const BenchmarkRun& median_run(const std::vector<BenchmarkRun>& runs) {
    std::vector<std::size_t> order(runs.size());
    for (std::size_t index = 0; index < runs.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&runs](std::size_t left, std::size_t right) {
        return runs[left].wall_time < runs[right].wall_time;
    });
    return runs[order[order.size() / 2]];
}

DepthSample median_sample(const std::vector<DepthSample>& samples) {
    std::vector<std::size_t> order(samples.size());
    for (std::size_t index = 0; index < samples.size(); ++index) {
        order[index] = index;
    }
    std::sort(order.begin(), order.end(), [&samples](std::size_t left, std::size_t right) {
        return samples[left].elapsed_ms < samples[right].elapsed_ms;
    });
    return samples[order[order.size() / 2]];
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

void write_run_metrics(std::ostream& output, const BenchmarkRun& run, bool timed) {
    const koi::SearchResult& result = run.result;
    const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
    const std::uint64_t elapsed = static_cast<std::uint64_t>(run.wall_time.count());
    const std::uint64_t nps = timed ? compute_nps(visited, elapsed) : 0;
    output << "\"score_cp\": " << result.score_cp << ", \"pv\": [";
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
    if (timed) {
        output << ", \"elapsed_ms\": " << run.wall_time.count();
    }
}

void write_profile_json(const std::string& path, const BenchmarkConfig& config,
                        const std::vector<PositionProfile>& profiles) {
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
           << "  \"warmup\": " << config.warmup << ",\n"
           << "  \"repeat\": " << config.repeat << ",\n"
           << "  \"node_limit\": " << config.node_limit << ",\n"
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
    for (std::size_t index = 0; index < profiles.size(); ++index) {
        const BenchPosition& position = profiles[index].benchmark;
        const BenchmarkRun& run = median_run(profiles[index].runs);
        output << "    {\"id\": ";
        write_json_string(output, position.name);
        output << ", \"fen\": ";
        write_json_string(output, position.fen);
        if (config.node_limit != 0) {
            output << ", \"limits\": {\"nodes\": " << config.node_limit << "}";
        } else {
            output << ", \"limits\": {\"depth\": " << static_cast<unsigned>(profiles[index].measured_depth) << "}";
        }
        output << ", \"expected_move\": ";
        write_json_string(output, position.expected_move);
        output << ", \"accepted_moves\": [";
        bool first_accepted_move = true;
        for (const std::string& accepted_move : position.accepted_moves) {
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
        write_json_string(output, position.category);
        output << ", \"hash_mb\": 512, \"hash_state\": \""
               << (config.warm_hash ? "warm" : "cold") << "\", \"threads\": " << config.threads
               << ", \"speed\": " << static_cast<unsigned>(config.speed_percent) << ", ";
        write_run_metrics(output, run, config.timed);
        if (profiles[index].runs.size() > 1) {
            output << ", \"runs\": [";
            for (std::size_t run_index = 0; run_index < profiles[index].runs.size(); ++run_index) {
                output << (run_index == 0 ? "{" : ", {");
                write_run_metrics(output, profiles[index].runs[run_index], config.timed);
                output << '}';
            }
            output << ']';
        }
        output << '}' << (index + 1 == profiles.size() ? '\n' : ',') << '\n';
    }
    output << "  ]\n}\n";
    if (!output) {
        throw std::runtime_error("failed while writing benchmark profile JSON");
    }
}

// koi-bench-speed-v1: the artifact consumed by tools/build/speed_gate.ps1.
// Every retained row is the median of the repeated runs for one
// (position, depth) pair, so alternating-run gates can compare NPS and
// time-to-depth without parsing the text report.
void write_speed_json(const std::string& path, const BenchmarkConfig& config,
                      const std::vector<PositionProfile>& profiles) {
    struct ReportRow {
        const BenchPosition* benchmark;
        DepthSample sample;
    };
    std::vector<ReportRow> rows;
    for (const PositionProfile& profile : profiles) {
        std::map<std::uint8_t, std::vector<DepthSample>> grouped;
        for (const BenchmarkRun& run : profile.runs) {
            for (const DepthSample& sample : run.samples) {
                grouped[sample.depth].push_back(sample);
            }
        }
        for (const auto& entry : grouped) {
            rows.push_back(ReportRow{&profile.benchmark, median_sample(entry.second)});
        }
    }

    std::uint64_t total_nodes = 0;
    std::uint64_t total_qnodes = 0;
    std::uint64_t total_elapsed = 0;
    for (const ReportRow& row : rows) {
        total_nodes += row.sample.nodes;
        total_qnodes += row.sample.qnodes;
        total_elapsed += row.sample.elapsed_ms;
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("unable to write speed report JSON");
    }

    output << "{\n  \"schema\": \"koi-bench-speed-v1\",\n"
           << "  \"engine\": \"Koi Engine\",\n"
           << "  \"build\": \"Koi Engine " KOI_ENGINE_BUILD_VERSION "\",\n"
           << "  \"suite\": \"" << suite_name(config) << "\",\n"
           << "  \"source\": ";
    write_json_string(output, config.fen_file_path ? *config.fen_file_path : std::string("built-in"));
    output << ",\n  \"evaluator\": \"" << (config.nnue_path ? "nnue" : "classical") << "\",\n"
           << "  \"threads\": " << config.threads << ",\n"
           << "  \"speed\": " << static_cast<unsigned>(config.speed_percent) << ",\n"
           << "  \"hash_mb\": 512,\n"
           << "  \"hash_state\": \"" << (config.warm_hash ? "warm" : "cold") << "\",\n"
           << "  \"timed\": " << (config.timed ? "true" : "false") << ",\n"
           << "  \"warmup\": " << config.warmup << ",\n"
           << "  \"repeat\": " << config.repeat << ",\n"
           << "  \"node_limit\": " << config.node_limit << ",\n"
           << "  \"depth\": ";
    if (config.fixed_depth.has_value()) {
        output << static_cast<unsigned>(*config.fixed_depth);
    } else {
        output << "null";
    }
    output << ",\n  \"depth_sweep\": ";
    if (config.depth_sweep.has_value()) {
        output << "{\"from\": " << static_cast<unsigned>(config.depth_sweep->first)
               << ", \"to\": " << static_cast<unsigned>(config.depth_sweep->second) << "}";
    } else {
        output << "null";
    }
    output << ",\n  \"positions\": [\n";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const BenchPosition& benchmark = *rows[index].benchmark;
        const DepthSample& sample = rows[index].sample;
        output << "    {\"id\": ";
        write_json_string(output, benchmark.name);
        output << ", \"fen\": ";
        write_json_string(output, benchmark.fen);
        output << ", \"depth\": " << static_cast<unsigned>(sample.depth)
               << ", \"nodes\": " << sample.nodes
               << ", \"qnodes\": " << sample.qnodes
               << ", \"tt_hits\": " << sample.tt_hits
               << ", \"score_cp\": " << sample.score_cp
               << ", \"best_move\": ";
        write_json_string(output, sample.best_move);
        output << ", \"elapsed_ms\": " << sample.elapsed_ms
               << ", \"nps\": " << sample.nps << '}';
        output << (index + 1 == rows.size() ? '\n' : ',') << '\n';
    }
    output << "  ],\n  \"totals\": {\"samples\": " << rows.size()
           << ", \"nodes\": " << total_nodes
           << ", \"qnodes\": " << total_qnodes
           << ", \"visited\": " << (total_nodes + total_qnodes)
           << ", \"elapsed_ms\": " << total_elapsed
           << ", \"nps\": " << compute_nps(total_nodes + total_qnodes, total_elapsed)
           << "}\n}\n";
    if (!output) {
        throw std::runtime_error("failed while writing speed report JSON");
    }
}

} // namespace

int main(int argc, char** argv) {
    auto config = parse_arguments(argc, argv);
    if (!config.has_value()) {
        std::cerr << "usage: koi-bench [--threads N] [--speed 1-100] [--timed] [--warm-hash] [--optional] [--nodes N] [--warmup K] [--repeat K] [--depth N] [--depth-sweep a..b] [--fen-file path] [--nnue path] [--profile-json path] [--report path]\n";
        return 2;
    }
    if (config->fen_file_path.has_value() && !config->depth_sweep.has_value() &&
        !config->fixed_depth.has_value() && config->node_limit == 0) {
        // External FEN lists carry no per-position depth, so fixed-depth mode
        // is the default measurement for them.
        config->fixed_depth = config->default_fen_depth;
    }

    try {
        std::cout << "Koi benchmark\n";
        std::cout << "config threads " << config->threads
                  << " speed " << static_cast<unsigned>(config->speed_percent)
                  << " timed " << (config->timed ? 1 : 0)
                  << " hash " << (config->warm_hash ? "warm" : "cold");
        if (config->optional && !config->fen_file_path.has_value()) {
            std::cout << " suite " << suite_name(*config);
        }
        if (config->node_limit != 0) {
            std::cout << " nodes " << config->node_limit;
        }
        if (config->warmup != 0) {
            std::cout << " warmup " << config->warmup;
        }
        if (config->repeat != 1) {
            std::cout << " repeat " << config->repeat;
        }
        if (config->fixed_depth.has_value()) {
            std::cout << " depth " << static_cast<unsigned>(*config->fixed_depth);
        }
        if (config->depth_sweep.has_value()) {
            std::cout << " depth-sweep " << static_cast<unsigned>(config->depth_sweep->first)
                      << ".." << static_cast<unsigned>(config->depth_sweep->second);
        }
        if (config->fen_file_path.has_value()) {
            std::cout << " fen-file " << *config->fen_file_path;
        }
        std::cout << '\n';
        std::vector<BenchPosition> positions = config->fen_file_path.has_value() ?
            load_fen_positions(*config->fen_file_path) : load_suite_positions(config->optional);
        for (BenchPosition& position : positions) {
            if (config->depth_sweep.has_value()) {
                // A sweep runs one iterative-deepening search to the top of
                // the range and retains every intermediate info sample.
                position.depth = config->depth_sweep->second;
            } else if (config->fixed_depth.has_value()) {
                position.depth = *config->fixed_depth;
            }
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
        const bool sweep = config->depth_sweep.has_value();
        const auto run_suite = [&](const bool record) {
            std::vector<PositionProfile> collected;
            if (record && (config->profile_json_path.has_value() ||
                           config->report_json_path.has_value())) {
                collected.reserve(positions.size());
            }
            for (const BenchPosition& benchmark : positions) {
                if (!config->warm_hash) {
                    // Reuse the 512 MB allocation while keeping every cold
                    // position independent of entries produced by its predecessor.
                    service.clear_hash();
                }
                const BenchmarkRun run = run_position(benchmark, *config, service);
                if (record) {
                    auto existing = std::find_if(collected.begin(), collected.end(),
                        [&benchmark](const PositionProfile& profile) {
                            return profile.benchmark.name == benchmark.name &&
                                profile.measured_depth == benchmark.depth;
                        });
                    if (existing == collected.end()) {
                        collected.push_back(PositionProfile{benchmark, benchmark.depth, {run}});
                    } else {
                        existing->runs.push_back(run);
                    }
                    if (sweep) {
                        for (const DepthSample& sample : run.samples) {
                            if (sample.depth < config->depth_sweep->first ||
                                sample.depth > config->depth_sweep->second) {
                                continue;
                            }
                            std::cout << "position " << benchmark.name
                                      << " depth " << static_cast<unsigned>(sample.depth)
                                      << " nodes " << sample.nodes
                                      << " qnodes " << sample.qnodes
                                      << " tt_hits " << sample.tt_hits
                                      << " score " << sample.score_cp
                                      << " move " << (sample.best_move.empty() ? "0000" : sample.best_move);
                            if (config->timed) {
                                std::cout << " elapsed_ms " << sample.elapsed_ms
                                          << " nps " << sample.nps;
                            }
                            std::cout << '\n';
                        }
                        continue;
                    }
                    const koi::SearchResult& result = run.result;
                    const bool matched = result.best_move.has_value() &&
                        accepts_move(benchmark, *result.best_move);
                    std::cout << "position " << benchmark.name
                              << " depth " << result.completed_depth
                              << " nodes " << result.stats.nodes
                              << " qnodes " << result.stats.qnodes
                              << " tt_hits " << result.stats.tt_hits
                              << " score " << result.score_cp;
                    if (!benchmark.expected_move.empty()) {
                        std::cout << " expected " << benchmark.expected_move
                                  << " move " << (result.best_move.has_value() ? result.best_move->uci() : "0000")
                                  << " match " << (matched ? 1 : 0);
                    } else {
                        std::cout << " move " << (result.best_move.has_value() ? result.best_move->uci() : "0000");
                    }
                    if (config->timed) {
                        const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
                        const std::uint64_t elapsed = static_cast<std::uint64_t>(run.wall_time.count());
                        std::cout << " elapsed_ms " << run.wall_time.count()
                                  << " nps " << compute_nps(visited, elapsed);
                    }
                    std::cout << '\n';
                }
            }
            return collected;
        };
        // Warmup passes keep the transposition table hot so the measured pass
        // reflects steady-state throughput rather than first-touch behavior.
        for (std::size_t pass = 0; pass < config->warmup; ++pass) {
            (void)run_suite(false);
        }
        std::vector<PositionProfile> profile_runs;
        for (std::size_t pass = 0; pass < config->repeat; ++pass) {
            std::vector<PositionProfile> collected = run_suite(true);
            if (pass == 0) {
                profile_runs = std::move(collected);
            } else {
                for (PositionProfile& profile : collected) {
                    auto existing = std::find_if(profile_runs.begin(), profile_runs.end(),
                        [&profile](const PositionProfile& candidate) {
                            return candidate.benchmark.name == profile.benchmark.name &&
                                candidate.measured_depth == profile.measured_depth;
                        });
                    if (existing != profile_runs.end()) {
                        existing->runs.insert(existing->runs.end(),
                                              profile.runs.begin(), profile.runs.end());
                    }
                }
            }
        }
        if (config->repeat > 1) {
            std::cout << "repeat " << config->repeat << " median summary\n";
            for (const PositionProfile& profile : profile_runs) {
                if (sweep) {
                    std::map<std::uint8_t, std::vector<DepthSample>> grouped;
                    for (const BenchmarkRun& run : profile.runs) {
                        for (const DepthSample& sample : run.samples) {
                            grouped[sample.depth].push_back(sample);
                        }
                    }
                    for (const auto& entry : grouped) {
                        const DepthSample& sample = median_sample(entry.second);
                        std::cout << "position " << profile.benchmark.name
                                  << " depth " << static_cast<unsigned>(sample.depth)
                                  << " nodes " << sample.nodes
                                  << " qnodes " << sample.qnodes
                                  << " score " << sample.score_cp
                                  << " elapsed_ms " << sample.elapsed_ms
                                  << " nps " << sample.nps
                                  << '\n';
                    }
                    continue;
                }
                const BenchmarkRun& run = median_run(profile.runs);
                const koi::SearchResult& result = run.result;
                const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
                const std::uint64_t elapsed = static_cast<std::uint64_t>(run.wall_time.count());
                std::cout << "position " << profile.benchmark.name
                          << " nodes " << result.stats.nodes
                          << " qnodes " << result.stats.qnodes
                          << " score " << result.score_cp
                          << " elapsed_ms " << run.wall_time.count()
                          << " nps " << compute_nps(visited, elapsed)
                          << '\n';
            }
        }
        if (config->profile_json_path.has_value()) {
            write_profile_json(*config->profile_json_path, *config, profile_runs);
        }
        if (config->report_json_path.has_value()) {
            write_speed_json(*config->report_json_path, *config, profile_runs);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
