#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"
#include "koi/cpu_variant.hpp"
#include "koi/executable_path.hpp"
#include "koi/nnue.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

namespace {

#if defined(__linux__) && defined(__GLIBC__)
// The Windows build links with a 16 MiB stack reserve that worker threads
// inherit.  glibc derives new thread stacks from RLIMIT_STACK (8 MiB by
// default) and ignores the linker's PT_GNU_STACK size, so raise the process
// default before any search thread exists; deep tactical lines rely on it.
void configure_worker_stack() noexcept {
    pthread_attr_t attributes;
    if (pthread_getattr_default_np(&attributes) != 0) {
        return;
    }
    if (pthread_attr_setstacksize(&attributes, 16U * 1024U * 1024U) == 0) {
        (void)pthread_setattr_default_np(&attributes);
    }
    pthread_attr_destroy(&attributes);
}
#endif

std::filesystem::path executable_directory(int argc, char* argv[]) {
    const std::filesystem::path module_path = koi::current_executable_path();
    if (!module_path.empty()) {
        return module_path.parent_path();
    }

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
#if defined(__linux__) && defined(__GLIBC__)
    configure_worker_stack();
#endif
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
#if defined(NDEBUG) && defined(KOI_CPU_REQUIRES_AVX512)
    if (!koi::cpu_supports_avx512()) {
        std::cerr << "Koi Engine Release requires an x64 CPU with AVX-512 support.\n";
        return 3;
    }
#endif
#if defined(NDEBUG) && defined(KOI_CPU_REQUIRES_AVX2)
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
    // Evaluator selection must never abort the process: a rejected network
    // (including one too large to allocate) falls back to the classical
    // evaluator.
    koi::EvaluatorSelection selection;
    try {
        selection = nnue_path.empty()
            ? koi::make_evaluator()
            : koi::make_evaluator(std::optional<std::filesystem::path>(nnue_path));
        if (selection.nnue_error.has_value()) {
            std::cerr << "koi-engine: NNUE network rejected (" << selection.nnue_error->message
                      << "); using the classical evaluator.\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "koi-engine: NNUE network rejected (" << error.what()
                  << "); using the classical evaluator.\n";
        selection = {};
    }
    if (selection.evaluator == nullptr) {
        try {
            selection = koi::make_evaluator();
        } catch (const std::exception& error) {
            std::cerr << "koi-engine: unable to construct an evaluator (" << error.what()
                      << ").\n";
            return 4;
        }
    }
    koi::SearchService search_service(selection.evaluator, {}, 1);
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  engine_directory);
    return controller.run();
}
