#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "koi/move.hpp"

namespace koi {

class SyzygyTablebase;

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

struct RootTimingContext {
    bool table_available = false;
    bool tt_hit = false;
    bool tt_exact = false;
    bool tt_has_best_move = false;
    int tt_depth = 0;
    std::uint16_t tt_generation_age = 0;
    std::uint32_t legal_move_count = 0;
    std::uint32_t forcing_move_count = 0;
    bool in_check = false;
};

struct SearchIterationObservation {
    int depth = 0;
    int score_cp = 0;
    bool best_move_changed = false;
    bool pv_changed = false;
    bool aspiration_researched = false;
    std::uint64_t nodes = 0;
};

struct TimeManagementStats {
    std::chrono::milliseconds reserve{0};
    std::chrono::milliseconds usable{0};
    std::chrono::milliseconds soft_budget{0};
    std::chrono::milliseconds hard_budget{0};
    std::uint32_t horizon = 20;
    int initial_hardness = 0;
    int observed_hardness = 0;
    bool emergency_pacing = false;
    bool extended_for_hard_position = false;
    bool hard_deadline_reached = false;
};

struct SearchStats {
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    // Worker-local qsearch cache hits. This is diagnostic-only; qsearch cache
    // entries are intentionally not shared through the regular TT because
    // their frontier includes q-depth and predecessor context.
    std::uint64_t qsearch_cache_hits = 0;
    // Number of full PositionFeatures snapshots requested by the search
    // context. This is diagnostic-only and helps keep expensive feature
    // extraction out of nodes that do not need it.
    std::uint64_t position_feature_extractions = 0;
    // Number of late-move feature checks served by a parent snapshot already
    // acquired for the current search node.
    std::uint64_t lmr_parent_feature_reuses = 0;
    // Number of evaluator calls served by the search-local static-evaluation cache.
    std::uint64_t evaluation_cache_hits = 0;
    std::uint64_t tt_hits = 0;
    std::uint64_t pvs_searches = 0;
    std::uint64_t pvs_researches = 0;
    std::uint64_t root_pvs_searches = 0;
    std::uint64_t root_pvs_researches = 0;
    std::uint64_t root_selective_candidates = 0;
    std::uint64_t root_selective_researches = 0;
    std::uint64_t short_fallback_invocations = 0;
    std::uint64_t short_fallback_candidates = 0;
    std::uint64_t short_fallback_overdue_candidates = 0;
    std::uint64_t quiet_forcing_extensions = 0;
    std::uint64_t aspiration_researches = 0;
    std::uint64_t check_extensions = 0;
    std::uint64_t king_safety_extensions = 0;
    std::uint64_t qchecks = 0;
    std::uint64_t see_prunes = 0;
    std::uint64_t delta_prunes = 0;
    std::uint64_t null_cutoffs = 0;
    std::uint64_t null_verifications = 0;
    std::uint64_t null_repetition_skips = 0;
    std::uint64_t lmr_reductions = 0;
    std::uint64_t lmr_verifications = 0;
    std::uint64_t lmr_king_zone_exclusions = 0;
    std::uint64_t lmr_high_history_exclusions = 0;
    std::uint64_t continuation_history_prunes = 0;
    std::uint64_t quiet_futility_prunes = 0;
    std::uint64_t reverse_futility_prunes = 0;
    std::uint64_t razoring_prunes = 0;
    std::uint64_t probcut_searches = 0;
    std::uint64_t probcut_cutoffs = 0;
    std::uint64_t singular_searches = 0;
    std::uint64_t singular_extensions = 0;
    std::uint64_t multi_cut_prunes = 0;
    std::uint64_t capture_history_updates = 0;
    std::uint64_t quiet_history_updates = 0;
    std::uint64_t continuation_history_updates = 0;
    std::uint64_t tbhits = 0;
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
    std::uint64_t tbhits = 0;
};

struct SearchRequestIdentity {
    std::uint64_t generation = 0;
    std::uint64_t root_key = 0;
    std::string root_fen;
};

struct SearchResult {
    std::optional<Move> best_move;
    std::vector<Move> pv;
    int score_cp = 0;
    std::optional<int> mate;
    int completed_depth = 0;
    SearchStats stats;
    TimeManagementStats timing;
    std::optional<Move> ponder_move;
    SearchRequestIdentity identity;
    bool completed = false;
    bool cancelled = false;
    bool failed = false;
};

struct SearchEventSink {
    std::function<void(const SearchInfo&)> on_info;
    std::function<void(const SearchResult&)> on_complete;
};

struct SearchOptions {
    using StrengthProfileHook = std::function<void(SearchOptions&)>;
    // Optional test/diagnostic seam for checking which side is used by quiet history.
    // It is not called unless explicitly configured, may run concurrently on root workers,
    // and exceptions are ignored by the search implementation.
    using QuietHistorySideHook = std::function<Color(Color candidate, bool after_unmake)>;

    // The portable engine default is intentionally sized for the supported
    // 32 GiB development/match machine.  UCI can still reduce this for small
    // GUI hosts, and the TT keeps its 1..4096 MiB safety bounds.
    std::size_t hash_mb = 512;
    std::size_t threads = 1;
    std::uint8_t speed_percent = 100;
    std::size_t multi_pv = 1;
    bool analyse_mode = false;
    bool show_wdl = false;
    std::uint32_t move_overhead_ms = 10;
    std::uint32_t slow_mover_percent = 100;
    bool limit_strength = false;
    std::uint32_t elo = 1320;
    std::uint64_t generation = 0;
    // Stable UCI snapshot for the future calibrated strength profile; currently neutral.
    bool strength_mode = false;
    std::shared_ptr<const SyzygyTablebase> syzygy;
    StrengthProfileHook strength_profile_hook;
    QuietHistorySideHook quiet_history_side_hook;
};

[[nodiscard]] inline std::size_t maximum_search_threads() noexcept {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::max<std::size_t>(1, std::min<std::size_t>(64, hardware == 0 ? 1 : hardware));
}

} // namespace koi
