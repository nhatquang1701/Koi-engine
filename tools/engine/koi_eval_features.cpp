// Dump per-position classical evaluation terms as CSV so the tuning tool can
// fit scalar weights offline.
//
// Input rows are either a bare `FEN` or the label-corpus format
// `FEN;cp;best_move`. The echoed `cp` is side-to-move relative, so the
// breakdown is always computed from the side to move, which matches the label
// corpus convention. The FEN field never contains a comma, so the output is
// plain CSV with the FEN first.

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

} // namespace

int main(int argc, char** argv) {
    std::string input_path;
    std::string output_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--input" && index + 1 < argc) {
            input_path = argv[++index];
        } else if (argument == "--output" && index + 1 < argc) {
            output_path = argv[++index];
        } else if (argument == "--help") {
            std::cout << "usage: koi-eval-features [--input FILE] [--output FILE]\n"
                         "  reads FEN or FEN;cp;... rows (stdin by default) and writes\n"
                         "  CSV term breakdowns from the side-to-move perspective\n";
            return 0;
        } else {
            std::cerr << "koi-eval-features: unknown argument " << argument << '\n';
            return 2;
        }
    }

    std::ifstream file_input;
    if (!input_path.empty()) {
        file_input.open(input_path);
        if (!file_input) {
            std::cerr << "koi-eval-features: cannot open " << input_path << '\n';
            return 2;
        }
    }
    std::istream* input = input_path.empty() ? &std::cin : &file_input;

    std::ofstream file_output;
    if (!output_path.empty()) {
        file_output.open(output_path, std::ios::trunc);
        if (!file_output) {
            std::cerr << "koi-eval-features: cannot write " << output_path << '\n';
            return 2;
        }
    }
    std::ostream* output = output_path.empty() ? &std::cout : &file_output;

    *output << "fen,cp,phase,material,piece_square,mobility,pawn_structure,activity,"
               "development,center_control,initiative,king_safety,king_activity,"
               "passed_pawn,tempo,total\n";

    const koi::ClassicalEvaluator evaluator;
    std::string line;
    while (std::getline(*input, line)) {
        line = strip_carriage_return(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find(';');
        const std::string_view fen =
            separator == std::string::npos ? std::string_view{line} : std::string_view{line}.substr(0, separator);
        std::string_view cp;
        if (separator != std::string::npos) {
            const std::size_t start = separator + 1;
            const std::size_t end = line.find(';', start);
            cp = std::string_view{line}.substr(start, end == std::string::npos ? std::string::npos : end - start);
        }
        if (fen.empty()) {
            continue;
        }
        const auto state = koi::GameState::from_fen(fen);
        if (!state.has_value()) {
            std::cerr << "koi-eval-features: skipped unparsable FEN " << fen << '\n';
            continue;
        }
        const koi::Color mover = state->side_to_move();
        const koi::EvaluationBreakdown breakdown = evaluator.breakdown(*state, mover);
        *output << fen << ',' << cp << ',' << static_cast<int>(state->position_features().game_phase) << ','
                << breakdown.material << ',' << breakdown.piece_square << ',' << breakdown.mobility << ','
                << breakdown.pawn_structure << ',' << breakdown.activity << ',' << breakdown.development << ','
                << breakdown.center_control << ',' << breakdown.initiative << ',' << breakdown.king_safety << ','
                << breakdown.king_activity << ',' << breakdown.passed_pawn << ',' << breakdown.tempo << ','
                << breakdown.total << '\n';
    }
    return 0;
}
