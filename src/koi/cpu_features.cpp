#include "koi/cpu_features.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace koi {

bool cpu_supports_avx2() noexcept {
#if defined(_MSC_VER)
    int maximum_leaf[4]{};
    __cpuid(maximum_leaf, 0);
    if (maximum_leaf[0] < 1) {
        return false;
    }

    int features[4]{};
    __cpuidex(features, 1, 0);
    const bool osxsave = (features[2] & (1 << 27)) != 0;
    const bool avx = (features[2] & (1 << 28)) != 0;
    if (!osxsave || !avx || (_xgetbv(0) & 0x6) != 0x6 || maximum_leaf[0] < 7) {
        return false;
    }

    int extended_features[4]{};
    __cpuidex(extended_features, 7, 0);
    return (extended_features[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}

bool cpu_supports_avx512() noexcept {
#if defined(_MSC_VER)
    int maximum_leaf[4]{};
    __cpuid(maximum_leaf, 0);
    if (maximum_leaf[0] < 7) {
        return false;
    }

    int features[4]{};
    __cpuidex(features, 1, 0);
    const bool osxsave = (features[2] & (1 << 27)) != 0;
    const bool avx = (features[2] & (1 << 28)) != 0;
    // XCR0 bits 1-2 enable XMM/YMM state, bits 5-7 the opmask, ZMM_Hi256 and
    // Hi16_ZMM state that every AVX-512 instruction needs.
    if (!osxsave || !avx || (_xgetbv(0) & 0xE6) != 0xE6) {
        return false;
    }

    int extended_features[4]{};
    __cpuidex(extended_features, 7, 0);
    constexpr int kAvx512Foundation = 16;
    constexpr int kAvx512DoublewordQuadword = 17;
    constexpr int kAvx512ConflictDetection = 28;
    constexpr int kAvx512ByteWord = 30;
    constexpr int kAvx512VectorLength = 31;
    const int required = (1 << kAvx512Foundation) | (1 << kAvx512DoublewordQuadword) |
                         (1 << kAvx512ConflictDetection) | (1 << kAvx512ByteWord) |
                         (1 << kAvx512VectorLength);
    return (extended_features[1] & required) == required;
#else
    return false;
#endif
}

} // namespace koi
