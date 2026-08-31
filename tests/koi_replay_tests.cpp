#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string quote_argument(std::string_view argument) {
    return '"' + std::string(argument) + '"';
}

std::map<std::string, std::string> run_replay(const std::filesystem::path& replay,
                                              std::string_view arguments) {
    const std::string command = '"' + quote_argument(replay.string()) + ' ' +
                                std::string(arguments) + '"';
    FILE* pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("unable to start koi-replay");
    }

    std::string output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int exit_code = _pclose(pipe);
    if (exit_code != 0) {
        throw std::runtime_error("koi-replay exited unsuccessfully: " + output);
    }

    std::map<std::string, std::string> fields;
    std::size_t offset = 0;
    while (offset < output.size()) {
        const std::size_t newline = output.find('\n', offset);
        const std::string line = output.substr(offset, newline - offset);
        const std::size_t separator = line.find(' ');
        if (separator != std::string::npos) {
            fields.emplace(line.substr(0, separator), line.substr(separator + 1));
        }
        if (newline == std::string::npos) {
            break;
        }
        offset = newline + 1;
    }
    return fields;
}

void require_field(const std::map<std::string, std::string>& fields, std::string_view field,
                   std::string_view expected) {
    const auto iterator = fields.find(std::string(field));
    if (iterator == fields.end() || iterator->second != expected) {
        throw std::runtime_error("expected " + std::string(field) + " " + std::string(expected));
    }
}

void test_startpos_replay(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "startpos moves e2e4 e7e5 g1f3");
    require_field(fields, "legal", "1");
    require_field(fields, "result", "*");
    require_field(fields, "termination", "ongoing");
    require_field(fields, "fen", "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2");
}

void test_castling_replay(const std::filesystem::path& replay) {
    const auto fields = run_replay(
        replay, "fen \"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1\" moves e1g1");
    require_field(fields, "legal", "1");
    require_field(fields, "fen", "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1");
}

void test_en_passant_replay(const std::filesystem::path& replay) {
    const auto fields = run_replay(
        replay, "fen \"rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2\" moves d5e6");
    require_field(fields, "legal", "1");
    require_field(fields, "fen", "rnbqkbnr/pppp1ppp/4P3/8/8/8/PPP1PPPP/RNBQKBNR b KQkq - 0 2");
}

void test_promotion_replay(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "fen \"4k3/P7/8/8/8/8/8/4K3 w - - 0 1\" moves a7a8q");
    require_field(fields, "legal", "1");
    require_field(fields, "fen", "Q3k3/8/8/8/8/8/8/4K3 b - - 0 1");
}

void test_illegal_move_does_not_corrupt_state(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "startpos moves e2e4 e2e5");
    require_field(fields, "legal", "0");
    require_field(fields, "termination", "illegal move");
    require_field(fields, "fen", "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
}

void test_checkmate_classification(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "fen \"7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\"");
    require_field(fields, "legal", "1");
    require_field(fields, "result", "1-0");
    require_field(fields, "termination", "checkmate");
}

void test_stalemate_classification(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "fen \"7k/5Q2/6K1/8/8/8/8/8 b - - 0 1\"");
    require_field(fields, "legal", "1");
    require_field(fields, "result", "1/2-1/2");
    require_field(fields, "termination", "stalemate");
}

void test_rule_draw_classification(const std::filesystem::path& replay) {
    const auto fields = run_replay(replay, "fen \"4k3/8/8/8/8/8/8/4K3 w - - 0 1\"");
    require_field(fields, "legal", "1");
    require_field(fields, "result", "1/2-1/2");
    require_field(fields, "termination", "rule draw");
}

struct TestCase {
    std::string_view name;
    void (*run)(const std::filesystem::path&);
};

} // namespace

int main(int argument_count, char* arguments[]) {
    if (argument_count != 2) {
        std::cerr << "usage: koi_replay_tests <koi-replay-path>\n";
        return 2;
    }

    const std::filesystem::path replay = arguments[1];
    const std::vector<TestCase> tests{
        {"startpos replay", test_startpos_replay},
        {"castling replay", test_castling_replay},
        {"en passant replay", test_en_passant_replay},
        {"promotion replay", test_promotion_replay},
        {"illegal move transaction", test_illegal_move_does_not_corrupt_state},
        {"checkmate classification", test_checkmate_classification},
        {"stalemate classification", test_stalemate_classification},
        {"rule draw classification", test_rule_draw_classification},
    };

    for (const TestCase& test : tests) {
        try {
            test.run(replay);
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
