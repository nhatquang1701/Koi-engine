#include "koi/uci_controller.hpp"

#include <charconv>
#include <sstream>
#include <string>
#include <vector>

namespace koi {

namespace {

std::vector<std::string> remaining_tokens(std::istream& command) {
    std::vector<std::string> tokens;
    for (std::string token; command >> token;) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

bool parse_random_seed(const std::string& value, std::uint32_t& seed) {
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seed);
    return error == std::errc{} && end == value.data() + value.size() && seed <= 2147483647U;
}

} // namespace

UciController::UciController(std::istream& input, std::ostream& output, std::ostream& diagnostics)
    : input_(input), output_(output), diagnostics_(diagnostics) {}

int UciController::run() {
    for (std::string line; std::getline(input_, line);) {
        std::istringstream command(line);
        std::string name;
        if (!(command >> name)) {
            continue;
        }

        if (name == "uci") {
            output_ << "id name Koi Engine\n"
                       "id author Koi Engine contributors\n"
                       "option name RandomSeed type spin default 0 min 0 max 2147483647\n"
                       "uciok\n"
                    << std::flush;
        } else if (name == "isready") {
            output_ << "readyok\n" << std::flush;
        } else if (name == "ucinewgame") {
            position_ = GameState::startpos();
        } else if (name == "position") {
            handle_position(command);
        } else if (name == "setoption") {
            handle_setoption(command);
        } else if (name == "go") {
            write_bestmove();
        } else if (name == "quit") {
            return 0;
        }
    }

    return 0;
}

void UciController::handle_position(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.empty()) {
        write_position_error("missing position");
        return;
    }

    GameState candidate = GameState::startpos();
    std::size_t next = 0;
    if (tokens[0] == "startpos") {
        next = 1;
    } else if (tokens[0] == "fen") {
        if (tokens.size() < 7) {
            write_position_error("invalid FEN");
            return;
        }

        std::string fen = tokens[1];
        for (std::size_t field = 2; field < 7; ++field) {
            fen += ' ';
            fen += tokens[field];
        }
        const auto parsed = GameState::from_fen(fen);
        if (!parsed) {
            write_position_error("invalid FEN");
            return;
        }
        candidate = *parsed;
        next = 7;
    } else {
        write_position_error("invalid position");
        return;
    }

    if (next < tokens.size()) {
        if (tokens[next] != "moves") {
            write_position_error("invalid position");
            return;
        }
        ++next;
        for (; next < tokens.size(); ++next) {
            const auto move = Move::parse_uci(tokens[next]);
            if (!move || !candidate.make_move(*move)) {
                write_position_error("invalid move");
                return;
            }
        }
    }

    position_ = std::move(candidate);
}

void UciController::handle_setoption(std::istream& command) {
    const std::vector<std::string> tokens = remaining_tokens(command);
    if (tokens.size() != 4 || tokens[0] != "name" || tokens[1] != "RandomSeed" ||
        tokens[2] != "value") {
        return;
    }

    std::uint32_t seed = 0;
    if (parse_random_seed(tokens[3], seed)) {
        chooser_.set_seed(seed);
    }
}

void UciController::write_bestmove() {
    output_ << "bestmove " << chooser_.choose(position_).uci() << '\n' << std::flush;
}

void UciController::write_position_error(const char* message) {
    output_ << "info string " << message << '\n' << std::flush;
    diagnostics_ << "UCI position error: " << message << '\n';
}

} // namespace koi
