module;
#include <array>
#include <cstdint>

export module koi:types;

export namespace koi::module_api {
inline constexpr unsigned architecture_version = 1;
inline constexpr unsigned types_boundary_version = 1;

enum class MoveKind : unsigned char {
    quiet,
    capture,
    en_passant,
    castling,
    promotion,
};

struct PackedMove {
    std::uint32_t value = 0;

    [[nodiscard]] constexpr bool is_null() const noexcept { return value == 0; }
};

struct SearchOptionsContract {
    std::uint32_t hash_mb = 512;
    std::uint32_t threads = 1;
    std::uint8_t speed_percent = 100;
    std::uint32_t multi_pv = 1;
    bool analyse_mode = false;
    bool show_wdl = false;
    std::uint32_t move_overhead_ms = 10;
    std::uint32_t slow_mover_percent = 100;
};

struct SearchLimitsContract {
    std::int32_t depth = 0;
    std::uint64_t node_budget = 0;
    std::uint64_t movetime_ms = 0;
    std::uint32_t moves_to_go = 0;
    bool has_depth = false;
    bool has_node_budget = false;
    bool has_movetime = false;
    bool infinite = false;
    bool ponder = false;
};

struct SearchStatisticsContract {
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    std::uint64_t tt_hits = 0;
    std::uint64_t tbhits = 0;
    std::uint32_t seldepth = 0;
};

struct SearchWorkerContextContract {
    std::uint32_t worker_id = 0;
    std::uint64_t nodes = 0;
    std::uint64_t qnodes = 0;
    bool cancelled = false;
};
}

// The native module boundary deliberately exports value contracts rather than
// implementation classes. Header façades remain available to existing UCI
// clients while the production position/search migration proceeds in slices.
