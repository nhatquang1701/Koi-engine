module;
#include <cstdint>

export module koi:search;

export import :eval;
export import :tablebase;

export namespace koi::module_api {
inline constexpr unsigned search_boundary_version = 1;

struct SearchSessionContract {
    std::uint64_t generation = 0;
    std::uint32_t thread_count = 1;
    std::uint64_t node_budget = 0;
    bool cancelled = false;
    bool running = false;
    bool completion_emitted = false;
};

struct RootCoordinatorContract {
    std::uint32_t root_move_count = 0;
    std::uint32_t completed_root_lines = 0;
    std::uint32_t multi_pv = 1;
    bool deterministic_tiebreak = true;
    bool full_window_root_search = true;
};

struct SearchResultContract {
    PackedMove best_move{};
    std::int32_t score_cp = 0;
    std::int32_t completed_depth = 0;
    SearchStatisticsContract statistics{};
    bool has_best_move = false;
};
}
