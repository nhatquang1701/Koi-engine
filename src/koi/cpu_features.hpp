#pragma once

namespace koi {

// Queries hardware and OS support without executing an AVX2 instruction.
// Release binaries use this before constructing the AVX2-optimized engine.
[[nodiscard]] bool cpu_supports_avx2() noexcept;

// Queries the AVX-512 F/CD/BW/DQ/VL subset that MSVC /arch:AVX512 may emit,
// including the XSAVE state for the opmask and ZMM registers, without executing
// an AVX-512 instruction. The AVX-512 Release binary checks this at startup.
[[nodiscard]] bool cpu_supports_avx512() noexcept;

} // namespace koi
