#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"
#include "koi/cpu_variant.hpp"
#include "koi/nnue.hpp"
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
#if defined(KOI_CPU_SELECTOR)
    // The baseline build runs on any x64 CPU. When a faster sibling executable
    // sits next to this binary and the host supports it, re-exec through that
    // binary so the AVX2 or AVX-512 code paths are used without asking the user
    // to pick a flavor. A missing or unstartable sibling keeps this process.
    const int selector_exit = koi::run_cpu_selector();
    if (selector_exit >= 0) {
        return selector_exit;
    }
#endif
#if defined(NDEBUG) && defined(_MSC_VER) && defined(KOI_CPU_REQUIRES_AVX512)
    if (!koi::cpu_supports_avx512()) {
        std::cerr << "Koi Engine Release requires an x64 CPU with AVX-512 support.\n";
        return 3;
    }
#endif
#if defined(NDEBUG) && defined(_MSC_VER) && defined(KOI_CPU_REQUIRES_AVX2)
    if (!koi::cpu_supports_avx2()) {
        std::cerr << "Koi Engine Release requires an x64 CPU with AVX2 support.\n";
        return 3;
    }
#endif
    // Bootstrap with a tiny table so a GUI/tournament can deliver its Hash
    // option before a large default allocation is charged to the first clock.
    // UciController materializes the configured 512 MB default at isready.
    //
    // Evaluator selection: a network named `koi.nnue` beside the executable
    // (or the path in KOI_NNUE_PATH) is activated automatically; otherwise the
    // classical evaluator stays in charge.  `setoption name EvalFile` can load
    // a different network at runtime, and a rejected file always falls back to
    // the classical evaluator instead of failing the engine.
    const std::filesystem::path engine_directory = executable_directory(argc, argv);
    std::filesystem::path nnue_path;
    if (const char* environment_path = std::getenv("KOI_NNUE_PATH");
        environment_path != nullptr && *environment_path != '\0') {
        nnue_path = environment_path;
    } else if (!engine_directory.empty()) {
        const std::filesystem::path candidate = engine_directory / "koi.nnue";
        std::error_code exists_error;
        if (std::filesystem::exists(candidate, exists_error) && !exists_error) {
            nnue_path = candidate;
        }
    }
    const koi::EvaluatorSelection selection =
        nnue_path.empty() ? koi::make_evaluator()
                          : koi::make_evaluator(std::optional<std::filesystem::path>(nnue_path));
    if (selection.nnue_error.has_value()) {
        std::cerr << "koi-engine: NNUE network rejected (" << selection.nnue_error->message
                  << "); using the classical evaluator.\n";
    }
    koi::SearchService search_service(selection.evaluator, {}, 1);
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  engine_directory);
    return controller.run();
}
