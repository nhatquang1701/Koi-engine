#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
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

bool accepts_move(const koi::StrengthPosition& position, std::string_view move) {
    return position.expected_move == move ||
        std::find(position.accepted_moves.begin(), position.accepted_moves.end(), move) !=
            position.accepted_moves.end();
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
        const bool has_allowlist = !position.expected_move.empty() ||
            std::any_of(position.accepted_moves.begin(), position.accepted_moves.end(),
                        [](std::string_view move) { return !move.empty(); });
        require(has_allowlist, "every hard tactical fixture must carry an accepted-move allowlist");
        if (!accepts_move(position, result->best_move->uci())) {
            throw std::runtime_error("unexpected move in " + std::string(position.name) +
                                     ": got " + result->best_move->uci());
        }
    }
}

void test_strength_suite_has_the_required_hard_gate_coverage() {
    const auto positions = koi::strength_positions();
    require(positions.size() == 64,
            "the deterministic hard tactical gate must contain exactly 64 positions");
    std::set<std::uint16_t> ids;
    for (const koi::StrengthPosition& position : positions) {
        require(position.id != 0 && !position.name.empty() && !position.category.empty(),
                "every hard tactical fixture must carry an ID, name, and category");
        ids.insert(position.id);
    }
    require(ids.size() == positions.size(), "hard tactical fixture IDs must be unique");
}

void test_optional_strength_corpus_has_required_categories_and_metadata() {
    const auto positions = koi::optional_strength_positions();
    require(positions.size() == 128, "the optional corpus must contain exactly 128 positions");
    std::set<std::uint16_t> ids;
    for (const koi::StrengthPosition& position : positions) {
        require(position.id != 0 && !position.name.empty() && !position.category.empty(),
                "every optional fixture must carry an ID and category");
        ids.insert(position.id);
        require(koi::GameState::from_fen(position.fen).has_value(),
                "every optional fixture must contain valid FEN");
        require(!position.expected_move.empty() ||
                    std::any_of(position.accepted_moves.begin(), position.accepted_moves.end(),
                                [](std::string_view move) { return !move.empty(); }),
                "every optional fixture must carry an accepted-move allowlist");
    }
    require(ids.size() == positions.size(), "optional corpus fixture IDs must be unique");
}

} // namespace

int main() {
    try {
        test_strength_suite_has_the_required_hard_gate_coverage();
        test_optional_strength_corpus_has_required_categories_and_metadata();
        test_strength_positions_are_valid_and_tactical_moves_are_found();
        std::cout << "PASS strength suite\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL strength suite: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
