#include "koi/executable_path.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace koi {

std::filesystem::path current_executable_path() noexcept {
#if defined(_WIN32)
    std::array<wchar_t, 32'768> buffer{};
    const DWORD length =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length != 0 && length < buffer.size()) {
        return std::filesystem::path(std::wstring_view(buffer.data(), length));
    }
    return {};
#elif defined(__linux__)
    std::array<char, PATH_MAX + 1> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<std::size_t>(length)] = '\0';
        return std::filesystem::path(buffer.data());
    }
    return {};
#elif defined(__APPLE__)
    // _NSGetExecutablePath returns the path used to launch the process, which
    // may be relative; canonicalize it so sibling lookups (koi.nnue, the CPU
    // variant executables) resolve beside the running binary.
    std::array<char, 4096> buffer{};
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code canonical_error;
    const std::filesystem::path path(buffer.data());
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, canonical_error);
    return canonical_error ? path : canonical;
#else
    return {};
#endif
}

} // namespace koi
