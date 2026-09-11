#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

namespace {

std::filesystem::path executable_directory(int argc, char* argv[]) {
#ifdef _WIN32
    std::array<wchar_t, 32'768> module_path{};
    const DWORD length = ::GetModuleFileNameW(nullptr, module_path.data(),
                                              static_cast<DWORD>(module_path.size()));
    if (length != 0 && length < module_path.size()) {
        return std::filesystem::path(std::wstring_view(module_path.data(), length)).parent_path();
    }
#endif

    std::error_code path_error;
    if (argc > 0) {
        const std::filesystem::path path = std::filesystem::absolute(argv[0], path_error);
        if (!path_error) {
            return path.parent_path();
        }
    }
    return {};
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(NDEBUG) && defined(_MSC_VER)
    if (!koi::cpu_supports_avx2()) {
        std::cerr << "Koi Engine Release requires an x64 CPU with AVX2 support.\n";
        return 3;
    }
#endif
    // Bootstrap with a tiny table so a GUI/tournament can deliver its Hash
    // option before a large default allocation is charged to the first clock.
    // UciController materializes the configured 512 MB default at isready.
    koi::SearchService search_service(std::make_shared<koi::ClassicalEvaluator>(), {}, 1);
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  executable_directory(argc, argv));
    return controller.run();
}
