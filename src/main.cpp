#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

int main(int argc, char* argv[]) {
    std::error_code path_error;
    std::filesystem::path executable_directory;
    if (argc > 0) {
        executable_directory = std::filesystem::absolute(argv[0], path_error).parent_path();
        if (path_error) {
            executable_directory.clear();
        }
    }
    koi::SearchService search_service(std::make_shared<koi::ClassicalEvaluator>());
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service),
                                  std::move(executable_directory));
    return controller.run();
}
