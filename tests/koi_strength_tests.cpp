#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <map>
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
    return std::find(position.accepted_moves.begin(), position.accepted_moves.end(), move) !=
        position.accepted_moves.end();
}

void validate_fixture_contract(std::span<const koi::StrengthPosition> positions,
                               std::size_t expected_count, bool hard_gate) {
    require(positions.size() == expected_count, "strength fixture corpus has an unexpected size");
    std::set<std::uint16_t> ids;
    std::set<std::string_view> fens;
    std::set<std::string_view> categories;
    std::map<std::string_view, std::size_t> category_counts;
    bool has_queen_capture = false;
    bool has_black_mate = false;

    for (const koi::StrengthPosition& position : positions) {
        require(position.id != 0 && !position.name.empty() && !position.category.empty(),
                "every fixture must carry an ID, name, and category");
        ids.insert(position.id);
        fens.insert(position.fen);
        categories.insert(position.category);
        ++category_counts[position.category];
        has_queen_capture = has_queen_capture || position.name == "queen_capture";
        has_black_mate = has_black_mate || position.name == "black_mate";

        const auto state = koi::GameState::from_fen(position.fen);
        require(state.has_value(), "every fixture must contain valid FEN");
        require(!position.expected_move.empty(), "every fixture must retain a deterministic expected move");
        require(std::find(position.accepted_moves.begin(), position.accepted_moves.end(),
                          position.expected_move) != position.accepted_moves.end(),
                "the expected move must appear in the explicit accepted-move allowlist");

        std::set<std::string_view> accepted;
        for (const std::string_view move_text : position.accepted_moves) {
            if (move_text.empty()) {
                continue;
            }
            accepted.insert(move_text);
            const auto move = koi::Move::parse_uci(move_text);
            require(move.has_value() && state->is_legal(*move),
                    "every accepted move must be legal for the fixture FEN");
            const auto metadata = state->describe_move(*move);
            require(metadata.has_value(), "every accepted move must have move metadata");
            if (position.category == "check") {
                require(metadata->gives_check, "check fixtures must accept only checking moves");
            }
            if (position.category == "promotion" || position.category == "pawn_race") {
                require(move->promotion() != koi::Promotion::none,
                        "promotion and pawn-race fixtures must accept a promotion move");
            }
            if (position.category == "poisoned_capture") {
                require(metadata->is_capture(),
                        "every poisoned-capture allowlist move must capture a piece");
            }
        }
        require(!accepted.empty(), "every fixture must carry an explicit accepted-move allowlist");
        require(accepted.size() == std::count_if(position.accepted_moves.begin(), position.accepted_moves.end(),
                                                  [](std::string_view move) { return !move.empty(); }),
                "accepted-move allowlists must not contain duplicate moves");
    }

    require(ids.size() == positions.size(), "fixture IDs must be unique");
    require(fens.size() == positions.size(), "fixture FENs must be genuinely distinct");
    if (hard_gate) {
        for (const std::string_view category : {"mate", "check", "evasion", "fork", "pin",
                                                 "poisoned_capture", "promotion", "defense", "pawn_race"}) {
            require(categories.contains(category), "hard gate is missing a required tactical category");
        }
        require(category_counts["mate"] == 8 && category_counts["check"] == 7 &&
                    category_counts["evasion"] == 7 && category_counts["fork"] == 7 &&
                    category_counts["pin"] == 7 && category_counts["poisoned_capture"] == 6 &&
                    category_counts["promotion"] == 7 && category_counts["defense"] == 7 &&
                    category_counts["pawn_race"] == 7 && category_counts["queen_capture"] == 1,
                "hard gate category counts must retain the reviewed 64-case distribution");
        require(has_queen_capture && has_black_mate,
                "hard gate must retain the legacy queen_capture and black_mate fixtures");
    } else {
        for (const std::string_view category : {"positional", "king_safety", "endgame"}) {
            require(categories.contains(category), "optional corpus is missing a required category");
        }
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
        if (position.category == "mate") {
            require(result->mate.has_value() && *result->mate > 0,
                    "mate fixtures must solve to a positive mate score");
        }
        if (position.category == "evasion") {
            require(state->in_check(), "evasion fixtures must begin with the side to move in check");
        }
        if (!accepts_move(position, result->best_move->uci())) {
            throw std::runtime_error("unexpected move in " + std::string(position.name) +
                                     ": got " + result->best_move->uci());
        }
    }
}

void test_strength_suite_has_the_required_hard_gate_coverage() {
    const auto positions = koi::strength_positions();
    validate_fixture_contract(positions, 64, true);
}

void test_optional_strength_corpus_has_required_categories_and_metadata() {
    const auto positions = koi::optional_strength_positions();
    validate_fixture_contract(positions, 128, false);
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
