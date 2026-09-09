#include <iostream>
#include <stdexcept>

#include "koi/cpu_features.hpp"

int main() {
    try {
        // The supported Windows x64 release target is AVX2.  The query must
        // be callable without executing an AVX2 instruction itself.
        if (!koi::cpu_supports_avx2()) {
            throw std::runtime_error("the configured release test host must support AVX2");
        }
        std::cout << "PASS AVX2 CPU feature query\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL AVX2 CPU feature query: " << error.what() << '\n';
        return 1;
    }
}
