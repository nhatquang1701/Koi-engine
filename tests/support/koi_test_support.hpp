#pragma once

// Shared test-support helpers for the Koi test layer.
//
// Every C++ test binary used to carry its own copy of these helpers. Link the
// `koi_test_support` INTERFACE target (see the root CMakeLists.txt) and include
// this header instead of redefining them locally.

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

// Absolute path of the test data directory, injected by CMake through the
// `koi_test_support` INTERFACE target. The fallback keeps the header usable
// when a translation unit is compiled without that definition.
#ifndef KOI_TEST_DATA_DIR
#define KOI_TEST_DATA_DIR "tests/data"
#endif

namespace koi::test {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

// Alias preserved for call sites that spell the assertion `require_state`.
inline void require_state(bool condition, std::string_view message) {
    require(condition, message);
}

// Resolve a path relative to the repository's `tests/data` directory. CMake
// registers the absolute directory so the result is independent of the current
// working directory and of `__FILE__` layouts (unity builds, relative paths).
inline std::filesystem::path fixture_path(std::string_view relative) {
    return std::filesystem::path(std::string(KOI_TEST_DATA_DIR)) /
        std::filesystem::path(std::string(relative));
}

} // namespace koi::test
