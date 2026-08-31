#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>

#include "koi/game_state.hpp"
#include "koi/perft.hpp"

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "usage: koi_perft <depth>\n";
        return 1;
    }

    int depth = 0;
    const std::string_view argument(argv[1]);
    const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), depth);
    if (error != std::errc{} || end != argument.data() + argument.size() || depth < 0) {
        std::cerr << "depth must be a non-negative integer\n";
        return 1;
    }

    koi::GameState state = koi::GameState::startpos();
    std::cout << "nodes " << koi::perft(state, depth) << '\n';
    return 0;
}
