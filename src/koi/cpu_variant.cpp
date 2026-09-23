#include "koi/cpu_variant.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "koi/cpu_features.hpp"
#include "koi/executable_path.hpp"

namespace koi {
namespace {

bool equals_ignore_case(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

#ifdef _WIN32

// Launches `variant` with the parent's standard handles and waits for it. The
// child is placed in a job object that dies with the parent so a GUI that kills
// `koi-engine.exe` cannot leave an orphaned search process behind. Returns the
// child's exit code, or -1 when the child could not be started at all.
int launch_variant(const std::filesystem::path& directory, CpuVariant variant,
                   bool explicit_choice) noexcept {
    const std::filesystem::path child_path =
        directory / std::filesystem::path(cpu_variant_executable_name(variant));
    std::wstring command_line = L"\"" + child_path.wstring() + L"\"";

    // Duplicate the standard handles so the child keeps working when the parent
    // inherited non-inheritable pipe handles from a GUI. When duplication is not
    // possible (no console at all), fall back to plain handle inheritance.
    const HANDLE sources[3] = {::GetStdHandle(STD_INPUT_HANDLE),
                               ::GetStdHandle(STD_OUTPUT_HANDLE),
                               ::GetStdHandle(STD_ERROR_HANDLE)};
    HANDLE duplicates[3] = {nullptr, nullptr, nullptr};
    bool duplicated = true;
    for (int index = 0; index < 3; ++index) {
        if (sources[index] == nullptr || sources[index] == INVALID_HANDLE_VALUE ||
            !::DuplicateHandle(::GetCurrentProcess(), sources[index], ::GetCurrentProcess(),
                               &duplicates[index], 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            duplicated = false;
            break;
        }
    }
    if (!duplicated) {
        for (HANDLE& handle : duplicates) {
            if (handle != nullptr) {
                ::CloseHandle(handle);
                handle = nullptr;
            }
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (duplicated) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = duplicates[0];
        startup.hStdOutput = duplicates[1];
        startup.hStdError = duplicates[2];
    }

    // While the selector waits, the child should own the console's Ctrl+C.
    const BOOL ignored_ctrl_c = ::SetConsoleCtrlHandler(nullptr, TRUE);

    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        (void)::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits));
    }

    PROCESS_INFORMATION process{};
    const BOOL created =
        ::CreateProcessW(child_path.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                         nullptr, directory.c_str(), &startup, &process);

    if (duplicated) {
        for (HANDLE& handle : duplicates) {
            ::CloseHandle(handle);
        }
    }

    if (!created) {
        const DWORD error = ::GetLastError();
        if (job != nullptr) {
            ::CloseHandle(job);
        }
        if (ignored_ctrl_c) {
            (void)::SetConsoleCtrlHandler(nullptr, FALSE);
        }
        if (explicit_choice) {
            std::fprintf(stderr, "koi-engine: could not start %s (error %lu).\n",
                         child_path.string().c_str(), static_cast<unsigned long>(error));
        }
        return -1;
    }

    if (job != nullptr) {
        (void)::AssignProcessToJobObject(job, process.hProcess);
    }

    (void)::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    (void)::GetExitCodeProcess(process.hProcess, &exit_code);

    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (job != nullptr) {
        ::CloseHandle(job);
    }
    if (ignored_ctrl_c) {
        (void)::SetConsoleCtrlHandler(nullptr, FALSE);
    }
    return static_cast<int>(exit_code);
}

#elif defined(__linux__)

// Replaces this process with `variant` through execv: the child keeps the
// standard streams, the process id, and the controlling terminal, so a GUI or
// tournament sees exactly one engine process. execv returns only when the child
// could not be started; the caller then keeps running the baseline build unless
// the variant was requested explicitly.
int launch_variant(const std::filesystem::path& directory, CpuVariant variant,
                   bool explicit_choice) noexcept {
    const std::filesystem::path child_path =
        directory / std::filesystem::path(cpu_variant_executable_name(variant));
    std::string path_text = child_path.string();
    char* arguments[] = {path_text.data(), nullptr};
    ::execv(path_text.c_str(), arguments);
    if (explicit_choice) {
        std::fprintf(stderr, "koi-engine: could not start %s.\n", path_text.c_str());
    }
    return -1;
}

#else

int launch_variant(const std::filesystem::path&, CpuVariant, bool) noexcept {
    return -1;
}

#endif // platform launch

} // namespace

std::optional<CpuVariant> parse_cpu_variant(std::string_view text) noexcept {
    text = trim(text);
    if (equals_ignore_case(text, "avx512")) {
        return CpuVariant::avx512;
    }
    if (equals_ignore_case(text, "avx2")) {
        return CpuVariant::avx2;
    }
    if (equals_ignore_case(text, "generic")) {
        return CpuVariant::generic;
    }
    return std::nullopt;
}

std::string_view cpu_variant_executable_name(CpuVariant variant) noexcept {
#ifdef _WIN32
    switch (variant) {
    case CpuVariant::generic:
        return "koi-engine.exe";
    case CpuVariant::avx2:
        return "koi-engine-avx2.exe";
    case CpuVariant::avx512:
        return "koi-engine-avx512.exe";
    }
    return "koi-engine.exe";
#else
    switch (variant) {
    case CpuVariant::generic:
        return "koi-engine";
    case CpuVariant::avx2:
        return "koi-engine-avx2";
    case CpuVariant::avx512:
        return "koi-engine-avx512";
    }
    return "koi-engine";
#endif
}

CpuVariant choose_cpu_variant(bool avx512_supported, bool avx2_supported,
                              bool avx512_available, bool avx2_available,
                              std::optional<CpuVariant> forced) noexcept {
    if (forced.has_value()) {
        switch (*forced) {
        case CpuVariant::generic:
            return CpuVariant::generic;
        case CpuVariant::avx512:
            if (avx512_available) {
                return CpuVariant::avx512;
            }
            break;
        case CpuVariant::avx2:
            if (avx2_available) {
                return CpuVariant::avx2;
            }
            break;
        }
    }
    if (avx512_supported && avx512_available) {
        return CpuVariant::avx512;
    }
    if (avx2_supported && avx2_available) {
        return CpuVariant::avx2;
    }
    return CpuVariant::generic;
}

int run_cpu_selector() noexcept {
    const std::filesystem::path executable = current_executable_path();
    if (executable.empty()) {
        return -1;
    }
    const std::filesystem::path directory = executable.parent_path();

    std::optional<CpuVariant> forced;
    if (const char* text = std::getenv("KOI_CPU_VARIANT"); text != nullptr && *text != '\0') {
        forced = parse_cpu_variant(text);
    }

    std::error_code exists_error;
    const bool avx512_available = std::filesystem::exists(
        directory / std::filesystem::path(cpu_variant_executable_name(CpuVariant::avx512)),
        exists_error);
    exists_error.clear();
    const bool avx2_available = std::filesystem::exists(
        directory / std::filesystem::path(cpu_variant_executable_name(CpuVariant::avx2)),
        exists_error);

    const CpuVariant chosen = choose_cpu_variant(cpu_supports_avx512(), cpu_supports_avx2(),
                                                 avx512_available, avx2_available, forced);
    if (chosen == CpuVariant::generic) {
        return -1;
    }
    return launch_variant(directory, chosen, forced.has_value());
}

} // namespace koi
