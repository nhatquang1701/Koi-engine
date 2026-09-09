module;
#include <cstdint>

export module koi:tablebase;

export import :position;

export namespace koi::module_api {
inline constexpr unsigned tablebase_boundary_version = 1;

struct TablebaseContract {
    std::uint8_t probe_limit = 5;
    std::uint8_t probe_depth = 1;
    bool enabled = false;
    bool fifty_move_rule = true;
    std::uint64_t hits = 0;
};
}
