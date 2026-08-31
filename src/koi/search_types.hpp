#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "koi/move.hpp"

namespace koi {

struct ClockLimit {
    std::chrono::milliseconds remaining{0};
    std::chrono::milliseconds increment{0};
};

struct SearchLimits {
    std::optional<int> depth;
    std::optional<std::uint64_t> nodes;
    std::optional<std::chrono::milliseconds> movetime;
    std::optional<ClockLimit> white_clock;
    std::optional<ClockLimit> black_clock;
    std::optional<std::uint32_t> moves_to_go;
    bool infinite = false;
    bool ponder = false;
    bool search_moves_specified = false;
    std::vector<Move> search_moves;
};

struct SearchStats {
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    std::chrono::milliseconds elapsed{0};
};

struct SearchInfo {
    int depth = 0;
    int score_cp = 0;
    std::optional<int> mate;
    std::uint64_t nodes = 0;
    std::uint64_t nps = 0;
    std::chrono::milliseconds elapsed{0};
    std::vector<Move> pv;
    int multipv = 1;
};

struct SearchResult {
    std::optional<Move> best_move;
    int score_cp = 0;
    std::optional<int> mate;
    int completed_depth = 0;
    SearchStats stats;
};

struct SearchEventSink {
    std::function<void(const SearchInfo&)> on_info;
    std::function<void(const SearchResult&)> on_complete;
};
    std::size_t multi_pv = 1;
    bool analyse_mode = false;

struct SearchOptions {
    std::size_t hash_mb = 16;
};

} // namespace koi
