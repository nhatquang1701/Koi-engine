#include <iostream>
#include <stdexcept>

#include "koi/cpu_features.hpp"

#include "koi_test_support.hpp"

namespace {

void test_avx2_cpu_feature_query() {
    // The supported Windows x64 release target is AVX2.  The query must
    // be callable without executing an AVX2 instruction itself.
    koi::test::require(koi::cpu_supports_avx2(),
                       "the configured release test host must support AVX2");
}

void test_avx512_cpu_feature_query() {
    // AVX-512 implies AVX2, never the other way around: Comet Lake, Alder
    // Lake, and Zen 3 expose AVX2 without AVX-512.  The query must be
    // callable without executing an AVX-512 instruction itself, so the test
    // only asserts the implication instead of requiring the host to have it.
    if (koi::cpu_supports_avx512()) {
        koi::test::require(koi::cpu_supports_avx2(),
                           "an AVX-512 host must also report AVX2 support");
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"AVX2 CPU feature query", test_avx2_cpu_feature_query},
        {"AVX-512 CPU feature query", test_avx512_cpu_feature_query},
    };
    return koi::test::run_tests(tests, argc, argv);
}
