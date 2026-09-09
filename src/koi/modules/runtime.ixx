module;
#include <cstdint>

export module koi:runtime;

export import :search;

export namespace koi::module_api {
inline constexpr unsigned runtime_boundary_version = 1;

struct UciRuntimeContract {
    unsigned architecture_version = koi::module_api::architecture_version;
    bool protocol_clean = true;
    bool single_completion = true;
    bool supports_ponder = true;
    bool supports_multi_pv = true;
    bool diagnostics_are_out_of_band = true;
};
}
