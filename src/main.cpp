#include <iostream>

#include "koi/uci_controller.hpp"

int main() {
    koi::UciController controller(std::cin, std::cout, std::cerr);
    return controller.run();
}
