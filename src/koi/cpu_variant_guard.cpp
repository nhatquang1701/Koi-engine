#include "koi/cpu_features.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(__GNUC__) || defined(__clang__)

namespace {

[[noreturn]] void exit_unsupported(const char* message) noexcept {
    std::fputs(message, stderr);
    std::fflush(stderr);
    std::_Exit(3);
}

} // namespace

// GCC and Clang vectorize the library's own static initializers with the
// instructions enabled by the build flags, so a variant binary can execute an
// unsupported instruction before main() ever runs its gate.  This constructor
// is compiled for the baseline instruction set and runs before the default
// priority (65535) static initializers, so an unsupported host still receives
// the gate message and exit code 3 instead of an illegal instruction.
__attribute__((constructor(101))) void koi_cpu_variant_guard() {
#if defined(KOI_CPU_REQUIRES_AVX512)
    if (!koi::cpu_supports_avx512()) {
        exit_unsupported("Koi Engine Release requires an x64 CPU with AVX-512 support.\n");
    }
#elif defined(KOI_CPU_REQUIRES_AVX2)
    if (!koi::cpu_supports_avx2()) {
        exit_unsupported("Koi Engine Release requires an x64 CPU with AVX2 support.\n");
    }
#endif
}

#endif
