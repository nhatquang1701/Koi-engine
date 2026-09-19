#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::vector<std::string> moves_for(std::string_view name) {
    if (name.contains("mate-white")) {
        // Scholar's mate: 1.e4 e5 2.Bc4 Nc6 3.Qh5 Nf6 4.Qxf7#.
        return {"e2e4", "f1c4", "d1h5", "h5f7"};
    }
    if (name.contains("passive-black")) {
        return {"e7e5", "b8c6", "g8f6"};
    }
    if (name.contains("white-repeater")) {
        return {"g1f3", "f3g1", "g1f3", "f3g1"};
    }
    if (name.contains("black-repeater")) {
        return {"g8f6", "f6g8", "g8f6", "f6g8"};
    }
    if (name.contains("illegal-koi")) {
        return {"0000"};
    }
    if (name.contains("clock-koi")) {
        return {"e2e4", "g1f3"};
    }
    if (name.contains("clock-opponent")) {
        return {"e7e5", "b8c6"};
    }
    if (name.contains("slow-koi")) {
        return {"e2e4"};
    }
    if (name.contains("clock-before-deadline-koi") || name.contains("clock-after-deadline-koi")) {
        return {"e2e4"};
    }
    return {"e7e5"};
}

} // namespace

int main(int argument_count, char* arguments[]) {
    const std::filesystem::path executable = argument_count > 0 ? arguments[0] : "fixture.exe";
    const std::string name = lower(executable.stem().string());
    const std::filesystem::path log_path = executable.parent_path() / (name + ".log");
    std::vector<std::string> moves = moves_for(name);
    std::size_t go_count = 0;
    std::size_t new_game_count = 0;

    std::string line;
    while (std::getline(std::cin, line)) {
        {
            std::ofstream log(log_path, std::ios::app);
            log << line << '\n';
        }
        if (line == "uci") {
            std::cout << "id name " << name << '\n';
            std::cout << "uciok\n" << std::flush;
        } else if (line == "isready") {
            std::cout << "readyok\n" << std::flush;
        } else if (line == "ucinewgame") {
            ++new_game_count;
            // Each game replays the script from the start; the alternating
            // SPRT fixture additionally picks its role by game parity.
            go_count = 0;
        } else if (line.starts_with("go ")) {
            if (name.contains("slow-koi") && go_count == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            if (name.contains("clock-before-deadline") && go_count == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(6));
            }
            if (name.contains("clock-after-deadline") && go_count == 0) {
                std::this_thread::sleep_for(std::chrono::seconds(61));
            }
            if (name.contains("clock-")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            std::vector<std::string> active_moves = moves;
            if (name.contains("sprt-alternator")) {
                // Odd games mate for the role's color, even games play passive
                // moves so the other side mates instead. Used by the SPRT
                // inconclusive-path process test.
                const bool mate_game = (new_game_count % 2) == 1;
                if (name.contains("white")) {
                    active_moves = mate_game
                        ? std::vector<std::string>{"e2e4", "f1c4", "d1h5", "h5f7"}
                        : std::vector<std::string>{"e2e4", "b2b3", "c2c3", "d2d3"};
                } else {
                    active_moves = mate_game
                        ? std::vector<std::string>{"a7a6", "b7b6", "c7c6"}
                        : std::vector<std::string>{"e7e5", "f8c5", "d8h4", "h4f2"};
                }
            }
            const std::size_t index = std::min(go_count, active_moves.size() - 1);
            if (name.contains("book-koi")) {
                std::cout << "info string book move " << active_moves[index] << " depth 1\n";
            }
            std::cout << "bestmove " << active_moves[index] << '\n' << std::flush;
            ++go_count;
        } else if (line == "quit") {
            break;
        }
    }

    return 0;
}
