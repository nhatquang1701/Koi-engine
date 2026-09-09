#pragma once

namespace koi {

// Queries hardware and OS support without executing an AVX2 instruction.
// Release binaries use this before constructing the AVX2-optimized engine.
[[nodiscard]] bool cpu_supports_avx2() noexcept;

} // namespace koi
