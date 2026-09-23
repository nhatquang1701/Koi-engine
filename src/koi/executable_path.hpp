#pragma once

#include <filesystem>

namespace koi {

// Absolute path of the running executable, or an empty path when it cannot be
// resolved. Windows asks the loader (GetModuleFileNameW); Linux reads the
// /proc/self/exe link, so the answer does not depend on argv[0] or PATH.
[[nodiscard]] std::filesystem::path current_executable_path() noexcept;

} // namespace koi
