#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/position.hpp"
#include "koi/uci_controller.hpp"

namespace {

using koi::Position;
using koi::UciController;

struct ControllerResult {
    int exit_code;
    std::string output;
    std::string diagnostics;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

ControllerResult run_controller(std::string_view transcript) {
    std::istringstream input{std::string(transcript)};
    std::ostringstream output;
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics);
    return {controller.run(), output.str(), diagnostics.str()};
}

std::vector<std::string> output_lines(std::string_view output) {
    std::vector<std::string> lines;
    std::istringstream stream{std::string(output)};
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

bool is_legal_move(const Position& position, std::string_view uci) {
    for (const auto& move : position.legal_moves()) {
        if (move.uci() == uci) {
            return true;
        }
    }
    return false;
}

void test_uci_handshake_has_the_required_order() {
    const ControllerResult result = run_controller("uci\nquit\n");

    require(result.exit_code == 0, "quit must cause a normal shutdown");
    require(result.output ==
                "id name Koi Engine\n"
                "id author Koi Engine contributors\n"
                "option name RandomSeed type spin default 0 min 0 max 2147483647\n"
                "uciok\n",
            "uci response must contain the required lines in order");
}

void test_isready_writes_readyok() {
    const ControllerResult result = run_controller("isready\nquit\n");

    require(result.output == "readyok\n", "isready must write readyok");
}

void test_reapplying_the_same_seed_repeats_the_best_move() {
    const ControllerResult result = run_controller(
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go\n"
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const Position initial;

    require(lines.size() == 2, "each go command must emit exactly one response");
    require(lines[0].starts_with("bestmove ") && lines[1].starts_with("bestmove "),
            "go responses must use bestmove lines");
    require(lines[0] == lines[1], "the same nonzero seed must repeat the selected move");
    require(is_legal_move(initial, lines[0].substr(9)), "the selected start-position move must be legal");
}

void test_startpos_and_fen_move_lists_define_the_position_for_go() {
    const ControllerResult result = run_controller(
        "setoption name RandomSeed value 9\n"
        "position startpos moves e2e4 e7e5 g1f3\n"
        "go\n"
        "position fen r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 moves e1g1\n"
        "go\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    Position after_startpos;
    Position after_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    require(after_startpos.apply_uci("e2e4") && after_startpos.apply_uci("e7e5") &&
                after_startpos.apply_uci("g1f3"),
            "test fixture start-position moves must be legal");
    require(after_fen.apply_uci("e1g1"), "test fixture FEN move must be legal");
    require(lines.size() == 2, "both go commands must receive one response");
    require(lines[0].starts_with("bestmove ") &&
                is_legal_move(after_startpos, lines[0].substr(9)),
            "startpos move list must be applied before choosing a move");
    require(lines[1].starts_with("bestmove ") && is_legal_move(after_fen, lines[1].substr(9)),
            "FEN move list must be applied before choosing a move");
}

void test_special_move_fen_still_returns_a_legal_bestmove() {
    const ControllerResult result = run_controller(
        "setoption name RandomSeed value 123\n"
        "position fen rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2\n"
        "go depth 8 wtime 1000 btime 1000 winc 0 binc 0 movestogo 20 movetime 50 infinite\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const Position en_passant("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");

    require(lines.size() == 1 && lines[0].starts_with("bestmove "),
            "go with accepted search tokens must emit one bestmove");
    require(is_legal_move(en_passant, lines[0].substr(9)),
            "a special-move FEN must use its legal move set");
    require(lines[0].find(" ponder ") == std::string::npos, "go must never emit a ponder move");
}

void test_invalid_position_commands_preserve_the_previous_position() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "position startpos moves e2e4 not-a-move\n"
        "position fen malformed\n"
        "go\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);

    require(lines.size() == 3, "each rejected position command must report one UCI error");
    require(lines[0].starts_with("info string ") && lines[1].starts_with("info string "),
            "invalid position input must report UCI info-string errors");
    require(lines[2] == "bestmove 0000", "failed position commands must leave checkmate unchanged");
}

void test_terminal_position_returns_0000() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go\n"
        "quit\n");

    require(result.output == "bestmove 0000\n", "a terminal position must return bestmove 0000");
}

void test_unknown_stop_and_blank_commands_are_quiet_and_quit() {
    const ControllerResult result = run_controller("\nunknown harmless command\nstop\nquit\n");

    require(result.exit_code == 0, "unknown and stop commands must not prevent quit");
    require(result.output.empty(), "blank, unknown, and stop commands must not write protocol output");
}

void test_protocol_output_contains_only_uci_responses() {
    const ControllerResult result = run_controller(
        "uci\n"
        "isready\n"
        "setoption name RandomSeed value 1\n"
        "position startpos\n"
        "go\n"
        "quit\n");

    for (const std::string& line : output_lines(result.output)) {
        require(line.starts_with("id ") || line.starts_with("option ") || line == "uciok" ||
                    line == "readyok" || line.starts_with("bestmove ") ||
                    line.starts_with("info string "),
                "stdout must contain only UCI protocol responses");
    }
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"uci handshake order", test_uci_handshake_has_the_required_order},
        {"ready response", test_isready_writes_readyok},
        {"deterministic seed", test_reapplying_the_same_seed_repeats_the_best_move},
        {"position startpos and FEN", test_startpos_and_fen_move_lists_define_the_position_for_go},
        {"special-move FEN", test_special_move_fen_still_returns_a_legal_bestmove},
        {"transactional invalid positions", test_invalid_position_commands_preserve_the_previous_position},
        {"terminal 0000", test_terminal_position_returns_0000},
        {"unknown stop quit", test_unknown_stop_and_blank_commands_are_quiet_and_quit},
        {"protocol-clean output", test_protocol_output_contains_only_uci_responses},
    };

    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }

    return 0;
}
