#include <iostream>
#include <memory>
#include <utility>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/uci_controller.hpp"

int main() {
    koi::SearchService search_service(std::make_shared<koi::ClassicalEvaluator>());
    koi::UciController controller(std::cin, std::cout, std::cerr, std::move(search_service));
    return controller.run();
}
