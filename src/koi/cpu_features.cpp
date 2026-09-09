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

} // namespace koi
