#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
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
    std::uint64_t pvs_searches = 0;
    std::uint64_t pvs_researches = 0;
    std::uint64_t aspiration_researches = 0;
    std::uint64_t check_extensions = 0;
    std::uint64_t qchecks = 0;
    std::uint64_t see_prunes = 0;
    std::uint64_t delta_prunes = 0;
    std::uint64_t null_cutoffs = 0;
    std::uint64_t lmr_reductions = 0;
    std::uint64_t lmr_verifications = 0;
    int seldepth = 0;
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
    int seldepth = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    int multipv = 1;
};

struct SearchResult {
    std::optional<Move> best_move;
    int score_cp = 0;
    std::optional<int> mate;
    int completed_depth = 0;
    SearchStats stats;
    std::optional<Move> ponder_move;
};

struct SearchEventSink {
    std::function<void(const SearchInfo&)> on_info;
    std::function<void(const SearchResult&)> on_complete;
};

struct SearchOptions {
    using StrengthProfileHook = std::function<void(SearchOptions&)>;

    std::size_t hash_mb = 16;
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    std::size_t multi_pv = 1;
    bool analyse_mode = false;
    bool show_wdl = false;
    std::uint32_t move_overhead_ms = 10;
    std::uint32_t slow_mover_percent = 100;
    bool limit_strength = false;
    std::uint32_t elo = 1320;
    StrengthProfileHook strength_profile_hook;
};

[[nodiscard]] inline std::size_t maximum_search_threads() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, std::min<std::size_t>(64, hardware == 0 ? 1 : hardware));
}

} // namespace koi
