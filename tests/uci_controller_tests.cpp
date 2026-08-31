#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
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

std::vector<std::string> lines_starting_with(const std::vector<std::string>& lines,
                                             std::string_view prefix) {
    std::vector<std::string> matches;
    for (const std::string& line : lines) {
        if (line.starts_with(prefix)) {
            matches.push_back(line);
        }
    }
    return matches;
}

std::size_t line_index(const std::vector<std::string>& lines, std::string_view expected) {
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index] == expected) {
            return index;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

bool is_legal_move(const Position& position, std::string_view uci) {
    for (const auto& move : position.legal_moves()) {
        if (move.uci() == uci) {
            return true;
        }
    }
    return false;
}

bool is_valid_search_info(std::string_view line) {
    std::istringstream stream{std::string(line)};
    std::string info;
    std::string depth_name;
    int depth = 0;
    std::string score_name;
    std::string score_kind;
    int score = 0;
    std::string nodes_name;
    std::uint64_t nodes = 0;
    std::string nps_name;
    std::uint64_t nps = 0;
    std::string time_name;
    std::uint64_t time = 0;
    std::string pv_name;

    if (!(stream >> info >> depth_name >> depth >> score_name >> score_kind >> score >>
          nodes_name >> nodes >> nps_name >> nps >> time_name >> time >> pv_name)) {
        return false;
    }
    if (info != "info" || depth_name != "depth" || depth <= 0 || score_name != "score" ||
        (score_kind != "cp" && score_kind != "mate") || nodes_name != "nodes" ||
        nps_name != "nps" || time_name != "time" || pv_name != "pv") {
        return false;
    }

    for (std::string move; stream >> move;) {
        if (move.size() != 4 && move.size() != 5) {
            return false;
        }
    }
    return true;
}

class FlushTrackingBuffer final : public std::stringbuf {
public:
    int sync() override {
        sync_count_.fetch_add(1, std::memory_order_relaxed);
        return std::stringbuf::sync();
    }

    [[nodiscard]] int sync_count() const noexcept {
        return sync_count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_int sync_count_ = 0;
};

void test_uci_handshake_has_identity_and_supported_options_in_order() {
    const ControllerResult result = run_controller("uci\nquit\n");

    require(result.exit_code == 0, "quit must cause a normal shutdown");
    require(result.output ==
                "id name Koi Engine\n"
                "id author Koi Engine contributors\n"
                "option name RandomSeed type spin default 0 min 0 max 2147483647\n"
                "option name Hash type spin default 16 min 1 max 4096\n"
                "option name Clear Hash type button\n"
                "uciok\n",
            "uci response must preserve identity and advertise RandomSeed, Hash, and Clear Hash");
    require(result.output.find("Threads") == std::string::npos,
            "Threads must not be advertised before parallel search exists");
}

void test_hash_options_preserve_the_contract_and_never_advertise_threads() {
    const ControllerResult result = run_controller(
        "uci\n"
        "setoption name Hash value 1\n"
        "setoption name Hash value 0\n"
        "setoption name Hash value 4097\n"
        "setoption name Clear Hash\n"
        "isready\n"
        "quit\n");

    require(result.exit_code == 0, "Hash and Clear Hash commands must leave the controller usable");
    require(result.output.find("option name Hash type spin default 16 min 1 max 4096\n") != std::string::npos,
            "the Hash option must retain its documented default and bounds");
    require(result.output.find("option name Clear Hash type button\n") != std::string::npos,
            "Clear Hash must remain a UCI button option");
    require(result.output.find("Threads") == std::string::npos,
            "Threads must remain absent until a parallel search implementation exists");
    require(result.output.ends_with("readyok\n"), "Hash option changes and Clear Hash must not disrupt isready");
}

void test_isready_writes_readyok() {
    const ControllerResult result = run_controller("isready\nquit\n");

    require(result.output == "readyok\n", "isready must write readyok");
}

void test_deterministic_search_repeats_the_best_move_with_compatibility_seed() {
    const ControllerResult result = run_controller(
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go depth 2\n"
        "stop\n"
        "setoption name RandomSeed value 42\n"
        "position startpos\n"
        "go depth 2\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    const Position initial;

    require(bestmoves.size() == 2, "each stopped go command must emit exactly one bestmove");
    require(bestmoves[0] == bestmoves[1], "deterministic search must repeat from the same root");
    require(is_legal_move(initial, bestmoves[0].substr(9)),
            "the deterministic start-position move must be legal");
}

void test_startpos_and_fen_move_lists_define_the_search_root() {
    const ControllerResult result = run_controller(
        "position startpos moves e2e4 e7e5 g1f3\n"
        "go depth 1\n"
        "stop\n"
        "position fen r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1 moves e1g1\n"
        "go depth 1\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    Position after_startpos;
    Position after_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");

    require(after_startpos.apply_uci("e2e4") && after_startpos.apply_uci("e7e5") &&
                after_startpos.apply_uci("g1f3"),
            "test fixture start-position moves must be legal");
    require(after_fen.apply_uci("e1g1"), "test fixture FEN move must be legal");
    require(bestmoves.size() == 2, "both go commands must receive one bestmove");
    require(is_legal_move(after_startpos, bestmoves[0].substr(9)),
            "startpos move list must be applied before searching");
    require(is_legal_move(after_fen, bestmoves[1].substr(9)),
            "FEN move list must be applied before searching");
}

void test_all_go_limits_and_malformed_values_are_accepted_without_crashing() {
    const ControllerResult result = run_controller(
        "position fen rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2\n"
        "go depth 1\nstop\n"
        "go nodes 0\nstop\n"
        "go movetime 0\nstop\n"
        "go wtime 0 btime 0 winc 0 binc 0 movestogo 1\nstop\n"
        "go infinite\nstop\n"
        "go depth bad nodes -1 movetime -1 wtime bad btime -1 winc nope binc -4 movestogo 0\nstop\n"
        "go\nstop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    const Position en_passant("rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");

    require(result.exit_code == 0, "valid and malformed go tokens must not crash the controller");
    require(bestmoves.size() == 7, "every stopped go variant must emit exactly one bestmove");
    for (const std::string& bestmove : bestmoves) {
        require(is_legal_move(en_passant, bestmove.substr(9)),
                "every supported-limit search must retain a legal root move");
        require(bestmove.find(" ponder ") == std::string::npos,
                "go must never emit an unsupported ponder move");
    }
}

void test_go_limit_parser_maps_each_supported_limit_exactly() {
    using namespace std::chrono_literals;

    const koi::SearchLimits limits = koi::uci::parse_go_limits(
        "depth 7 nodes 18446744073709551615 movetime 0 "
        "wtime 600000 btime 123456 winc 3500 binc 17 movestogo 40");

    require(limits.depth == 7, "depth must map to SearchLimits::depth");
    require(limits.nodes == std::numeric_limits<std::uint64_t>::max(),
            "nodes must preserve the full uint64 range");
    require(limits.movetime == 0ms, "movetime must map to milliseconds exactly");
    require(limits.white_clock.has_value() && limits.white_clock->remaining == 600000ms &&
                limits.white_clock->increment == 3500ms,
            "wtime and winc must map to the white clock exactly");
    require(limits.black_clock.has_value() && limits.black_clock->remaining == 123456ms &&
                limits.black_clock->increment == 17ms,
            "btime and binc must map to the black clock exactly");
    require(limits.moves_to_go == 40, "movestogo must map to SearchLimits::moves_to_go");
    require(!limits.infinite, "ordinary limits must not enable infinite search");
}

void test_go_limit_parser_uses_depth_one_for_missing_malformed_overflow_and_asymmetric_clock_values() {
    const std::vector<std::string_view> commands{
        "",
        "depth nodes movetime wtime btime winc binc movestogo",
        "depth 0 nodes nope movetime -1 wtime bad btime -1 winc nope binc -4 movestogo 0",
        "depth 2147483648 nodes 18446744073709551616 movetime 9223372036854775808 "
        "wtime 9223372036854775808 btime 9223372036854775808 "
        "winc 9223372036854775808 binc 9223372036854775808 movestogo 4294967296",
        "wtime 1000",
        "btime 1000 binc 25",
    };

    for (const std::string_view command : commands) {
        const koi::SearchLimits limits = koi::uci::parse_go_limits(command);

        require(limits.depth == 1, "an unusable go command must fall back to depth one");
        require(!limits.nodes.has_value() && !limits.movetime.has_value(),
                "invalid node and movetime values must be ignored");
        require(!limits.white_clock.has_value() && !limits.black_clock.has_value(),
                "invalid clock values must not create clock limits");
        require(!limits.moves_to_go.has_value() && !limits.infinite,
                "invalid movestogo and absent infinite must remain unset");
    }
}

void test_invalid_position_commands_preserve_the_previous_position() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "position startpos moves e2e4 not-a-move\n"
        "position fen malformed\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> errors = lines_starting_with(lines, "info string ");
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");

    require(errors.size() == 2, "each rejected position command must report one UCI error");
    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "failed position commands must leave checkmate unchanged");
}

void test_terminal_position_returns_0000() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "a terminal position must return exactly one bestmove 0000");
}

void test_isready_remains_responsive_during_infinite_search() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "isready\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> lines = output_lines(result.output);
    const std::vector<std::string> bestmoves = lines_starting_with(lines, "bestmove ");
    const std::size_t ready = line_index(lines, "readyok");
    const std::size_t bestmove = bestmoves.empty() ? std::numeric_limits<std::size_t>::max()
                                                   : line_index(lines, bestmoves[0]);

    require(ready < bestmove, "isready must respond before stop completes an infinite search");
    require(bestmoves.size() == 1, "stop must emit exactly one final bestmove");
}

void test_position_replacement_suppresses_the_stale_generation() {
    const ControllerResult result = run_controller(
        "position startpos\n"
        "go infinite\n"
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "stop\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");

    require(bestmoves.size() == 1 && bestmoves[0] == "bestmove 0000",
            "position replacement must suppress the prior generation and duplicate stops");
}

void test_ucinewgame_and_hash_changes_suppress_active_generations() {
    const ControllerResult result = run_controller(
        "position fen 7k/6Q1/5K2/8/8/8/8/8 b - - 0 1\n"
        "go infinite\n"
        "ucinewgame\n"
        "go infinite\n"
        "setoption name Hash value 32\n"
        "go infinite\n"
        "setoption name Clear Hash\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    const std::vector<std::string> bestmoves =
        lines_starting_with(output_lines(result.output), "bestmove ");
    const Position initial;

    require(bestmoves.size() == 1,
            "ucinewgame, Hash, and Clear Hash must suppress each replaced generation");
    require(is_legal_move(initial, bestmoves[0].substr(9)),
            "the surviving post-option search must use the reset start position");
}

void test_quit_and_eof_join_without_late_bestmove() {
    const ControllerResult quit = run_controller("position startpos\ngo infinite\nquit\n");
    const ControllerResult eof = run_controller("position startpos\ngo infinite\n");

    require(quit.exit_code == 0 && eof.exit_code == 0,
            "quit and EOF must both shut down an active worker cleanly");
    require(lines_starting_with(output_lines(quit.output), "bestmove ").empty(),
            "quit must suppress the active generation's late bestmove");
    require(lines_starting_with(output_lines(eof.output), "bestmove ").empty(),
            "EOF must suppress output while joining the active worker");
}

void test_unknown_stop_and_blank_commands_are_quiet_and_quit() {
    const ControllerResult result = run_controller("\nunknown harmless command\nstop\nquit\n");

    require(result.exit_code == 0, "unknown and idle stop commands must not prevent quit");
    require(result.output.empty(), "blank, unknown, and idle stop commands must be quiet");
}

void test_protocol_output_contains_only_valid_uci_responses() {
    const ControllerResult result = run_controller(
        "uci\n"
        "isready\n"
        "setoption name RandomSeed value 1\n"
        "position startpos\n"
        "go depth 2\n"
        "stop\n"
        "quit\n");

    for (const std::string& line : output_lines(result.output)) {
        const bool valid = line.starts_with("id ") || line.starts_with("option ") ||
            line == "uciok" || line == "readyok" || line.starts_with("bestmove ") ||
            line.starts_with("info string ") || is_valid_search_info(line);
        require(valid, "stdout must contain only valid UCI protocol responses");
    }
    require(result.diagnostics.empty(), "a valid transcript must leave stderr clean");
}

void test_protocol_responses_flush_promptly() {
    std::istringstream input(
        "uci\n"
        "isready\n"
        "position startpos moves not-a-move\n"
        "go infinite\n"
        "stop\n"
        "quit\n");
    FlushTrackingBuffer output_buffer;
    std::ostream output(&output_buffer);
    std::ostringstream diagnostics;
    UciController controller(input, output, diagnostics);

    require(controller.run() == 0, "flush test transcript must shut down normally");
    require(output_buffer.sync_count() >= 4,
            "handshake, readyok, position error, and bestmove must each flush promptly");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"uci handshake and options", test_uci_handshake_has_identity_and_supported_options_in_order},
        {"Hash and Clear Hash contract", test_hash_options_preserve_the_contract_and_never_advertise_threads},
        {"ready response", test_isready_writes_readyok},
        {"deterministic search", test_deterministic_search_repeats_the_best_move_with_compatibility_seed},
        {"position startpos and FEN", test_startpos_and_fen_move_lists_define_the_search_root},
        {"go limits and malformed values", test_all_go_limits_and_malformed_values_are_accepted_without_crashing},
        {"go limit parser exact mapping", test_go_limit_parser_maps_each_supported_limit_exactly},
        {"go limit parser fallback", test_go_limit_parser_uses_depth_one_for_missing_malformed_overflow_and_asymmetric_clock_values},
        {"transactional invalid positions", test_invalid_position_commands_preserve_the_previous_position},
        {"terminal 0000", test_terminal_position_returns_0000},
        {"ready during search", test_isready_remains_responsive_during_infinite_search},
        {"position generation replacement", test_position_replacement_suppresses_the_stale_generation},
        {"new game and hash generation replacement", test_ucinewgame_and_hash_changes_suppress_active_generations},
        {"quit and EOF cleanup", test_quit_and_eof_join_without_late_bestmove},
        {"unknown stop quit", test_unknown_stop_and_blank_commands_are_quiet_and_quit},
        {"protocol-clean output", test_protocol_output_contains_only_valid_uci_responses},
        {"promptly flushed responses", test_protocol_responses_flush_promptly},
    };

    int failures = 0;
    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
