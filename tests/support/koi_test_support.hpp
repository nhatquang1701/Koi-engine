#pragma once

// Shared test-support harness for the Koi test layer.
//
// Every C++ test binary used to carry its own copy of these helpers plus a
// near-identical `main()` loop. Link the `koi_test_support` INTERFACE target
// (see the root CMakeLists.txt) and include this header instead.
//
// A migrated executable ends in:
//
//     int main(int argc, char** argv) {
//         const std::vector<koi::test::TestCase> tests{ ... };
//         return koi::test::run_tests(tests, argc, argv);
//     }
//
// Supported command line arguments (all optional):
//     --list            print every case name and exit 0
//     --filter=<text>   run only cases whose name contains <text>
//     --shard=<i>/<n>   run a deterministic 0-based shard of the selected cases
//     --quiet           suppress per-case PASS lines (FAIL/XFAIL/SKIP always print)
//
// Supported environment variables:
//     KOI_TEST_FILTER      substring filter (overridden by --filter)
//     KOI_TEST_SHARD       "i/n" shard selection (overridden by --shard)
//     KOI_TEST_RETRIES     attempts for cases listed in timing_sensitive() (default 1)
//     KOI_ALLOW_XPASS      when "1", an unexpected pass of a known failure is reported
//                          but does not fail the run (transitional escape hatch)
//
// Result contract (stable, machine-readable):
//     PASS <name>
//     FAIL <name>: <message>
//     XFAIL <name>: <message>
//     XPASS <name>: <message>
//     SKIP <name>: <reason>
//     XFAIL-UNSEEN <name>
//     koi-test-summary run=<n> pass=<n> fail=<n> xfail=<n> xpass=<n> skip=<n> unseen=<n> intermittent=<n>
//
// An unexpected pass (XPASS) fails the run so stale known-failure entries are
// pruned; the summary line is printed even when cases fail.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <expected>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

// Absolute path of the test data directory, injected by CMake through the
// `koi_test_support` INTERFACE target. The fallback keeps the header usable
// when a translation unit is compiled without that definition.
#ifndef KOI_TEST_DATA_DIR
#define KOI_TEST_DATA_DIR "tests/data"
#endif

namespace koi::test {

// ---------------------------------------------------------------------------
// Assertions and value helpers
// ---------------------------------------------------------------------------

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

// Alias preserved for call sites that spell the assertion `require_state`.
inline void require_state(bool condition, std::string_view message) {
    require(condition, message);
}

// Assert that an optional-like value holds a result and return it by move.
template <typename T>
inline T require_value(std::optional<T> value, std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return std::move(*value);
}

// Same contract for engine APIs that report failures through std::expected.
template <typename T, typename E>
inline T require_value(std::expected<T, E> value, std::string_view message) {
    if (!value.has_value()) {
        throw std::runtime_error(std::string(message));
    }
    return std::move(*value);
}

// Resolve a path relative to the repository's `tests/data` directory. CMake
// registers the absolute directory so the result is independent of the current
// working directory and of `__FILE__` layouts (unity builds, relative paths).
inline std::filesystem::path fixture_path(std::string_view relative) {
    return std::filesystem::path(std::string(KOI_TEST_DATA_DIR)) /
        std::filesystem::path(std::string(relative));
}

// ---------------------------------------------------------------------------
// Skips
// ---------------------------------------------------------------------------

// Thrown to mark a case as skipped: the case is environment-dependent (for
// example it needs at least two search threads) and asserts nothing. Skips are
// counted in the summary line instead of silently passing.
class TestSkip final : public std::exception {
public:
    explicit TestSkip(std::string reason) : reason_(std::move(reason)) {}
    [[nodiscard]] const char* what() const noexcept override { return reason_.c_str(); }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

private:
    std::string reason_;
};

[[noreturn]] inline void skip(std::string_view reason) {
    throw TestSkip(std::string(reason));
}

// ---------------------------------------------------------------------------
// Environment helpers
// ---------------------------------------------------------------------------

inline std::optional<std::string_view> environment_value(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return std::nullopt;
    }
    return std::string_view(raw);
}

inline std::size_t environment_uint(const char* name, std::size_t fallback) {
    const auto raw = environment_value(name);
    if (!raw.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(*raw), &consumed);
        if (consumed != raw->size()) {
            return fallback;
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

inline bool environment_flag(const char* name, bool fallback = false) {
    const auto raw = environment_value(name);
    if (!raw.has_value()) {
        return fallback;
    }
    return *raw == "1" || *raw == "true" || *raw == "on" || *raw == "yes";
}

// ---------------------------------------------------------------------------
// Unique temporary directories
// ---------------------------------------------------------------------------

// A process-unique scratch directory removed recursively on destruction. Tests
// must use this instead of fixed names so the suite stays safe under
// `ctest -j` and concurrent Debug/Release trees.
class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t unique = counter.fetch_add(1, std::memory_order_relaxed);
        const std::uint64_t stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
#ifdef _WIN32
        const auto process = static_cast<unsigned long long>(_getpid());
#else
        const auto process = static_cast<unsigned long long>(::getpid());
#endif
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        path_ = base / ("koi-test-" + std::to_string(process) + "-" +
                        std::to_string(stamp) + "-" + std::to_string(unique));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    TempDirectory(TempDirectory&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }

    TempDirectory& operator=(TempDirectory&& other) noexcept {
        if (this != &other) {
            remove();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    ~TempDirectory() { remove(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / std::filesystem::path(std::string(name));
    }

    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    void remove() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        path_.clear();
    }

    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

struct TestCase {
    std::string_view name;
    void (*run)();
};

struct TestRunOptions {
// Cases whose expectations are not met yet. They report XFAIL; a pass
// reports XPASS and fails the run (unless KOI_ALLOW_XPASS=1).
std::span<const std::string_view> known_failures{};
// Cases whose outcome depends on host scheduling, so they pass or fail from
// run to run. Both outcomes are reported (XFAIL/XPASS) but neither is fatal;
// keep this list shrinking and never move a deterministic gap here.
std::span<const std::string_view> intermittent{};

    // Cases that depend on wall-clock scheduling. KOI_TEST_RETRIES controls how
    // many attempts they get before a failure is final (default 1 attempt).
    std::span<const std::string_view> timing_sensitive{};
};

namespace detail {

struct Selection {
    std::optional<std::string_view> filter;
    std::size_t shard_index = 0;
    std::size_t shard_count = 1;
    bool quiet = false;
    bool list = false;
};

inline std::optional<std::size_t> parse_size(std::string_view text) {
    try {
        std::size_t consumed = 0;
        const auto value = std::stoull(std::string(text), &consumed);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

inline Selection parse_arguments(int argc, char** argv) {
    Selection selection;
    if (const auto from_environment = environment_value("KOI_TEST_FILTER"); from_environment.has_value()) {
        selection.filter = from_environment;
    }
    if (const auto shard = environment_value("KOI_TEST_SHARD"); shard.has_value()) {
        const auto separator = shard->find('/');
        if (separator != std::string_view::npos) {
            const auto index = parse_size(shard->substr(0, separator));
            const auto count = parse_size(shard->substr(separator + 1));
            if (index.has_value() && count.has_value() && *count > 0 && *index < *count) {
                selection.shard_index = *index;
                selection.shard_count = *count;
            }
        }
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index] != nullptr ? argv[index] : "");
        if (argument == "--list") {
            selection.list = true;
        } else if (argument == "--quiet" || argument == "-q") {
            selection.quiet = true;
        } else if (argument.rfind("--filter=", 0) == 0) {
            selection.filter = argument.substr(std::string_view("--filter=").size());
        } else if (argument.rfind("--shard=", 0) == 0) {
            const auto value = argument.substr(std::string_view("--shard=").size());
            const auto separator = value.find('/');
            if (separator != std::string_view::npos) {
                const auto shard_index = parse_size(value.substr(0, separator));
                const auto shard_count = parse_size(value.substr(separator + 1));
                if (shard_index.has_value() && shard_count.has_value() &&
                    *shard_count > 0 && *shard_index < *shard_count) {
                    selection.shard_index = *shard_index;
                    selection.shard_count = *shard_count;
                }
            }
        }
    }
    return selection;
}

inline bool contains(std::span<const std::string_view> values, std::string_view name) {
    return std::find(values.begin(), values.end(), name) != values.end();
}

} // namespace detail

inline int run_tests(std::span<const TestCase> tests, int argc = 0, char** argv = nullptr,
                     const TestRunOptions& options = {}) {
    const detail::Selection selection = detail::parse_arguments(argc, argv);

    if (selection.list) {
        std::size_t selected = 0;
        for (std::size_t index = 0; index < tests.size(); ++index) {
            const TestCase& test = tests[index];
            if (selection.filter.has_value() &&
                test.name.find(*selection.filter) == std::string_view::npos) {
                continue;
            }
            if (index % selection.shard_count != selection.shard_index) {
                continue;
            }
            std::cout << "case " << test.name << '\n';
            ++selected;
        }
        std::cout << "koi-test-summary list=" << selected << '\n';
        return 0;
    }

    const std::size_t retries = std::max<std::size_t>(1, environment_uint("KOI_TEST_RETRIES", 1));
    const bool allow_xpass = environment_flag("KOI_ALLOW_XPASS", false);

    std::size_t run = 0;
    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t xfailed = 0;
    std::size_t xpassed = 0;
    std::size_t intermittent_observed = 0;
    std::size_t skipped = 0;
    std::vector<std::string_view> seen_known_failures;

    for (std::size_t index = 0; index < tests.size(); ++index) {
        const TestCase& test = tests[index];
        const std::string_view name = test.name;
        if (selection.filter.has_value() &&
            name.find(*selection.filter) == std::string_view::npos) {
            continue;
        }
        if (index % selection.shard_count != selection.shard_index) {
            continue;
        }
        ++run;

        const bool intermittent = detail::contains(options.intermittent, name);
        const bool known_failure = detail::contains(options.known_failures, name) || intermittent;
        if (known_failure) {
            seen_known_failures.push_back(name);
        }
        const std::size_t attempts =
            detail::contains(options.timing_sensitive, name) ? retries : 1;

        bool succeeded = false;
        std::string message;
        bool case_skipped = false;
        for (std::size_t attempt = 1; attempt <= attempts; ++attempt) {
            try {
                test.run();
                succeeded = true;
                break;
            } catch (const TestSkip& skip_reason) {
                case_skipped = true;
                message = skip_reason.reason();
                break;
            } catch (const std::exception& error) {
                message = error.what();
            } catch (...) {
                message = "unknown non-standard exception";
            }
            if (attempt < attempts) {
                std::cout << "RETRY " << name << " (attempt " << attempt << "/" << attempts
                          << "): " << message << '\n';
            }
        }

        if (case_skipped) {
            std::cout << "SKIP " << name << ": " << message << '\n';
            ++skipped;
            continue;
        }
        if (succeeded) {
            if (known_failure) {
                if (intermittent) {
                    std::cout << "XPASS " << name
                              << ": intermittent known failure now passes\n";
                    ++xpassed;
                    ++intermittent_observed;
                } else {
                    std::cout << "XPASS " << name
                              << ": known failure now passes; remove it from the known_failures list\n";
                    if (allow_xpass) {
                        ++xpassed;
                    } else {
                        ++failed;
                    }
                }
            } else {
                ++passed;
                if (!selection.quiet) {
                    std::cout << "PASS " << name << '\n';
                }
            }
            continue;
        }
        if (known_failure) {
            std::cout << "XFAIL " << name << ": " << message << '\n';
            ++xfailed;
        } else {
            std::cerr << "FAIL " << name << ": " << message << '\n';
            ++failed;
        }
    }

    std::size_t unseen = 0;
    const bool full_selection = !selection.filter.has_value() && selection.shard_count == 1;
    if (full_selection) {
        for (const std::string_view known : options.known_failures) {
            if (std::find(seen_known_failures.begin(), seen_known_failures.end(), known) ==
                seen_known_failures.end()) {
                std::cerr << "XFAIL-UNSEEN " << known << '\n';
                ++unseen;
                ++failed;
            }
        }
    }

    std::cout << "koi-test-summary run=" << run << " pass=" << passed << " fail=" << failed
              << " xfail=" << xfailed << " xpass=" << xpassed << " skip=" << skipped
              << " unseen=" << unseen << " intermittent=" << intermittent_observed << '\n';
    return failed == 0 ? 0 : 1;
}

} // namespace koi::test
