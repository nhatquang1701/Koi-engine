#include "koi/cpu_features.hpp"

#include <cstdint>

// CPUID/XGETBV exist only on x86.  On arm64 (Apple Silicon, and any other
// non-x86 target) both queries are constants: the instruction sets do not
// exist, and the engine stays on its scalar paths.
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define KOI_CPU_ARCH_X86 1
#elif (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#define KOI_CPU_ARCH_X86 1
#else
#define KOI_CPU_ARCH_X86 0
#endif

namespace koi {
namespace {

#if KOI_CPU_ARCH_X86 && (defined(__GNUC__) || defined(__clang__))
// Reads XCR0 through inline assembly.  The probe cannot use
// __builtin_cpu_supports(): this translation unit is compiled once per CPU
// flavor, and a compiler may fold that builtin to a constant when the feature
// is enabled by the build flags (Clang does so for -mavx2 and -mavx512*),
// which would report the build's instruction set instead of the host's.
// CPUID/XGETBV always describe the running CPU.
[[nodiscard]] std::uint64_t read_xcr0() noexcept {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32U) | low;
}
#endif

} // namespace

bool cpu_supports_avx2() noexcept {
#if !KOI_CPU_ARCH_X86
    return false;
#elif defined(_MSC_VER)
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
#elif defined(__GNUC__) || defined(__clang__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    const unsigned int maximum_leaf = __get_cpuid_max(0, nullptr);
    if (maximum_leaf < 1U) {
        return false;
    }

    __cpuid(1U, eax, ebx, ecx, edx);
    const bool osxsave = (ecx & (1U << 27U)) != 0;
    const bool avx = (ecx & (1U << 28U)) != 0;
    if (!osxsave || !avx || (read_xcr0() & 0x6U) != 0x6U || maximum_leaf < 7U) {
        return false;
    }

    __cpuid_count(7U, 0U, eax, ebx, ecx, edx);
    return (ebx & (1U << 5U)) != 0;
#else
    return false;
#endif
}

bool cpu_supports_avx512() noexcept {
#if !KOI_CPU_ARCH_X86
    return false;
#elif defined(_MSC_VER)
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
#elif defined(__GNUC__) || defined(__clang__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7U) {
        return false;
    }

    __cpuid(1U, eax, ebx, ecx, edx);
    const bool osxsave = (ecx & (1U << 27U)) != 0;
    const bool avx = (ecx & (1U << 28U)) != 0;
    if (!osxsave || !avx || (read_xcr0() & 0xE6U) != 0xE6U) {
        return false;
    }

    __cpuid_count(7U, 0U, eax, ebx, ecx, edx);
    constexpr unsigned int kAvx512Foundation = 16U;
    constexpr unsigned int kAvx512DoublewordQuadword = 17U;
    constexpr unsigned int kAvx512ConflictDetection = 28U;
    constexpr unsigned int kAvx512ByteWord = 30U;
    constexpr unsigned int kAvx512VectorLength = 31U;
    const unsigned int required = (1U << kAvx512Foundation) | (1U << kAvx512DoublewordQuadword) |
                                  (1U << kAvx512ConflictDetection) | (1U << kAvx512ByteWord) |
                                  (1U << kAvx512VectorLength);
    return (ebx & required) == required;
#else
    return false;
#endif
}

bool cpu_supports_fast_pext() noexcept {
#if !KOI_CPU_ARCH_X86
    return false;
#elif defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    const bool intel = registers[1] == 0x756E6547 && registers[2] == 0x49656E69 &&
        registers[3] == 0x6C65746E;  // "GenuineIntel"
    const bool amd = registers[1] == 0x68747541 && registers[2] == 0x69746E65 &&
        registers[3] == 0x444D4163;  // "AuthenticAMD"
    if (registers[0] < 7) {
        return false;
    }
    __cpuidex(registers, 7, 0);
    if ((registers[1] & (1 << 8)) == 0) {  // BMI2
        return false;
    }
    __cpuid(registers, 1);
    const unsigned base_family = (static_cast<unsigned>(registers[0]) >> 8U) & 0xFU;
    const unsigned family = base_family == 0xFU ?
        base_family + ((static_cast<unsigned>(registers[0]) >> 20U) & 0xFFU) : base_family;
    if (intel) {
        return true;
    }
    if (amd) {
        return family >= 0x19U;  // Zen 3 and newer; Zen 1/2 are family 0x17
    }
    return false;
#else
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid_max(0, nullptr) < 7U) {
        return false;
    }
    __cpuid(0U, eax, ebx, ecx, edx);
    const bool intel = ebx == 0x756E6547 && edx == 0x49656E69 && ecx == 0x6C65746E;
    const bool amd = ebx == 0x68747541 && edx == 0x69746E65 && ecx == 0x444D4163;
    __cpuid_count(7U, 0U, eax, ebx, ecx, edx);
    if ((ebx & (1U << 8)) == 0) {  // BMI2
        return false;
    }
    __cpuid(1U, eax, ebx, ecx, edx);
    const unsigned base_family = (eax >> 8U) & 0xFU;
    const unsigned family = base_family == 0xFU ? base_family + ((eax >> 20U) & 0xFFU) : base_family;
    if (intel) {
        return true;
    }
    if (amd) {
        return family >= 0x19U;
    }
    return false;
#endif
}

} // namespace koi
