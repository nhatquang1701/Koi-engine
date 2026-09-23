#include <optional>
#include <string>
#include <vector>

#include "koi/cpu_variant.hpp"

#include "koi_test_support.hpp"

using koi::CpuVariant;
using koi::choose_cpu_variant;
using koi::cpu_variant_executable_name;
using koi::parse_cpu_variant;

namespace {

void test_parse_cpu_variant() {
    koi::test::require(parse_cpu_variant("avx512") == CpuVariant::avx512,
                       "avx512 must parse to the AVX-512 variant");
    koi::test::require(parse_cpu_variant("AVX2") == CpuVariant::avx2,
                       "parsing must ignore case");
    koi::test::require(parse_cpu_variant(" generic ") == CpuVariant::generic,
                       "parsing must trim surrounding whitespace");
    koi::test::require(!parse_cpu_variant("auto").has_value(),
                       "auto must not force a variant");
    koi::test::require(!parse_cpu_variant("").has_value(),
                       "an empty value must not force a variant");
    koi::test::require(!parse_cpu_variant("avx").has_value(),
                       "an unknown value must not force a variant");
}

void test_executable_names() {
    const std::string suffix =
#if defined(_WIN32)
        ".exe";
#else
        "";
#endif
    koi::test::require(cpu_variant_executable_name(CpuVariant::generic) ==
                           "koi-engine" + suffix,
                       "the baseline build keeps the primary engine name");
    koi::test::require(cpu_variant_executable_name(CpuVariant::avx2) ==
                           "koi-engine-avx2" + suffix,
                       "the AVX2 build must expose its own name");
    koi::test::require(cpu_variant_executable_name(CpuVariant::avx512) ==
                           "koi-engine-avx512" + suffix,
                       "the AVX-512 build must expose its own name");
}

void test_choose_cpu_variant_prefers_the_fastest_supported_sibling() {
    const std::optional<CpuVariant> automatic;
    koi::test::require(choose_cpu_variant(true, true, true, true, automatic) ==
                           CpuVariant::avx512,
                       "an AVX-512 host with both siblings must select AVX-512");
    koi::test::require(choose_cpu_variant(true, true, false, true, automatic) ==
                           CpuVariant::avx2,
                       "a missing AVX-512 sibling must fall back to AVX2");
    koi::test::require(choose_cpu_variant(true, true, false, false, automatic) ==
                           CpuVariant::generic,
                       "missing siblings must fall back to the baseline build");
    koi::test::require(choose_cpu_variant(false, true, true, true, automatic) ==
                           CpuVariant::avx2,
                       "a host without AVX-512 must select AVX2");
    koi::test::require(choose_cpu_variant(false, false, true, true, automatic) ==
                           CpuVariant::generic,
                       "a host with neither extension must select the baseline build");
}

void test_choose_cpu_variant_honors_an_explicit_override() {
    koi::test::require(choose_cpu_variant(true, true, true, true, CpuVariant::generic) ==
                           CpuVariant::generic,
                       "the generic override must always be honored");
    koi::test::require(choose_cpu_variant(false, false, true, true, CpuVariant::avx512) ==
                           CpuVariant::avx512,
                       "a forced AVX-512 build must be launched even when the query "
                       "disagrees; the target gates itself");
    koi::test::require(choose_cpu_variant(true, true, true, false, CpuVariant::avx2) ==
                           CpuVariant::avx512,
                       "a forced variant with no executable must fall back to the "
                       "best supported build");
    koi::test::require(choose_cpu_variant(false, true, false, false, CpuVariant::avx2) ==
                           CpuVariant::generic,
                       "a forced variant with no executable and no usable sibling "
                       "must use the baseline build");
    koi::test::require(choose_cpu_variant(true, true, true, true, CpuVariant::avx2) ==
                           CpuVariant::avx2,
                       "the AVX2 override must be honored when its executable exists");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"CPU variant parsing", test_parse_cpu_variant},
        {"CPU variant executable names", test_executable_names},
        {"CPU variant automatic selection", test_choose_cpu_variant_prefers_the_fastest_supported_sibling},
        {"CPU variant explicit override", test_choose_cpu_variant_honors_an_explicit_override},
    };
    return koi::test::run_tests(tests, argc, argv);
}
