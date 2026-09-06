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
    const std::vector<std::string> moves = moves_for(name);
    std::size_t go_count = 0;

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
            const std::size_t index = std::min(go_count, moves.size() - 1);
            if (name.contains("book-koi")) {
                std::cout << "info string book move " << moves[index] << " depth 1\n";
            }
            std::cout << "bestmove " << moves[index] << '\n' << std::flush;
            ++go_count;
        } else if (line == "quit") {
            break;
        }
    }

    return 0;
}
