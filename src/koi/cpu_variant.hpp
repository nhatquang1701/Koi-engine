#pragma once

#include <optional>
#include <string_view>

namespace koi {

// The CPU-specific engine builds that can be selected at startup.
enum class CpuVariant {
    generic,
    avx2,
    avx512,
};

// Parses `KOI_CPU_VARIANT`: `auto`, `generic`, `avx2`, or `avx512`
// (case-insensitive). `auto` and unknown text both mean "no override".
[[nodiscard]] std::optional<CpuVariant> parse_cpu_variant(std::string_view text) noexcept;

// Names the executable that implements a variant, for example
// "koi-engine-avx2.exe".
[[nodiscard]] std::string_view cpu_variant_executable_name(CpuVariant variant) noexcept;

// Picks the variant for the running host. `avx512_supported` and
// `avx2_supported` describe the CPU; `avx512_available` and `avx2_available`
// say whether the matching sibling executable exists next to the running
// binary. A forced variant is honored whenever its executable is present (the
// target binary still gates itself), otherwise the best supported variant wins.
[[nodiscard]] CpuVariant choose_cpu_variant(bool avx512_supported, bool avx2_supported,
                                            bool avx512_available, bool avx2_available,
                                            std::optional<CpuVariant> forced) noexcept;

// Startup selector for the baseline engine. Returns -1 when the caller must
// continue in-process (the generic build is the right choice, the faster
// sibling is missing, or launching it failed) or the child's exit code after a
// successful re-exec. Windows only; other platforms return -1.
[[nodiscard]] int run_cpu_selector() noexcept;

} // namespace koi
