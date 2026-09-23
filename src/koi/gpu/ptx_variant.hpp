#pragma once

// PTX architecture selection for the GPU NNUE kernel.
//
// The build embeds one PTX module per compute capability (see the generated
// `koi_nnue_v5_ptx.hpp`) so a single binary covers the GPUs from Pascal through
// Blackwell.  PTX can be JIT-compiled forward to newer architectures but never
// backwards, so a device runs the newest embedded module whose compute
// capability does not exceed the device's own.

#include <cstddef>
#include <span>

namespace koi::gpu {

struct NnueV5PtxVariant {
    int major;
    int minor;
    const char* source;
};

[[nodiscard]] constexpr bool ptx_variant_supports(const NnueV5PtxVariant& variant,
                                                  int device_major,
                                                  int device_minor) noexcept {
    return variant.major < device_major ||
        (variant.major == device_major && variant.minor <= device_minor);
}

[[nodiscard]] constexpr bool ptx_variant_newer(const NnueV5PtxVariant& left,
                                               const NnueV5PtxVariant& right) noexcept {
    return left.major > right.major ||
        (left.major == right.major && left.minor > right.minor);
}

// Returns the index of the newest variant the device can run, or -1 when the
// device is older than every embedded variant (no GPU path, CPU fallback).
[[nodiscard]] constexpr int select_ptx_variant(std::span<const NnueV5PtxVariant> variants,
                                               int device_major,
                                               int device_minor) noexcept {
    int best = -1;
    for (std::size_t index = 0; index < variants.size(); ++index) {
        const NnueV5PtxVariant& variant = variants[index];
        if (!ptx_variant_supports(variant, device_major, device_minor)) {
            continue;
        }
        if (best < 0 ||
            ptx_variant_newer(variant, variants[static_cast<std::size_t>(best)])) {
            best = static_cast<int>(index);
        }
    }
    return best;
}

} // namespace koi::gpu
