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
    koi::SearchService search_service(std::make_shared<koi::ClassicalEvaluator>());
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  executable_directory(argc, argv));
    return controller.run();
}
