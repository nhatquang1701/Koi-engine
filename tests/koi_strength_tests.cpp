#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "koi/classical_evaluator.hpp"
#include "koi/search_service.hpp"
#include "koi/strength_suite.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_strength_positions_are_valid_and_tactical_moves_are_found() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    for (const koi::StrengthPosition& position : koi::strength_positions()) {
        const auto state = koi::GameState::from_fen(position.fen);
        require(state.has_value(), "every strength fixture must contain valid FEN");
        koi::SearchLimits limits;
        limits.depth = position.depth;
        std::optional<koi::SearchResult> result;
        koi::SearchHandle handle = service.start(*state, limits,
            {.on_complete = [&result](const koi::SearchResult& completed) {
                result = completed;
            }});
        handle.wait();
        require(result.has_value() && result->best_move.has_value(),
                "every strength fixture must produce a best move");
        if (result->best_move->uci() != position.expected_move) {
            throw std::runtime_error("unexpected move in " + std::string(position.name) +
                                     ": expected " + std::string(position.expected_move) +
                                     ", got " + result->best_move->uci());
        }
    }
}

} // namespace

int main() {
    try {
        test_strength_positions_are_valid_and_tactical_moves_are_found();
        std::cout << "PASS strength suite\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL strength suite: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
