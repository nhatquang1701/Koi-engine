module;
#include <cstdint>

export module koi:tablebase;

export import :position;

export namespace koi::module_api {
inline constexpr unsigned tablebase_boundary_version = 2;

struct TablebaseContract {
    std::uint8_t probe_limit = 7;
    std::uint8_t probe_depth = 1;
    // Interior probing is opt-in and off by default; probe_depth keeps gating
    // the root probe regardless of this value.
    std::uint8_t interior_depth = 0;
    // Largest table registered in the loaded directory (0 when disabled).
    std::uint8_t large_table_limit = 0;
    bool enabled = false;
    bool fifty_move_rule = true;
    std::uint64_t hits = 0;
};
}
