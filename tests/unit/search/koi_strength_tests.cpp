#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

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

std::size_t color_index(koi::Color color) {
    return color == koi::Color::white ? 0U : 1U;
}

bool is_attacked(const koi::PositionFeatures& features, koi::Color color, koi::Square square) {
    return square.index() < koi::Square::kInvalid &&
        ((features.attacked_squares[color_index(color)] >> square.index()) & 1ULL) != 0;
}

std::array<unsigned, 12> material_signature(const koi::GameState& state) {
    std::array<unsigned, 12> signature{};
    for (std::uint8_t index = 0; index < 64; ++index) {
        const koi::Piece piece = state.piece_at(koi::Square::from_index(index));
        if (piece.empty() || piece.type == koi::PieceType::none) {
            continue;
        }
        const unsigned type = static_cast<unsigned>(piece.type);
        const unsigned color = color_index(piece.color);
        signature[color * 6U + type - 1U] += 1U;
    }
    return signature;
}

bool lies_between_on_line(koi::Square source, koi::Square target, koi::Square king) {
    const int source_file = source.file() - 'a';
    const int source_rank = source.rank() - '1';
    const int target_file = target.file() - 'a';
    const int target_rank = target.rank() - '1';
    const int king_file = king.file() - 'a';
    const int king_rank = king.rank() - '1';
    const int source_to_target_file = target_file - source_file;
    const int source_to_target_rank = target_rank - source_rank;
    const int target_to_king_file = king_file - target_file;
    const int target_to_king_rank = king_rank - target_rank;
    const auto sign = [](int value) { return (value > 0) - (value < 0); };
    const bool source_target_line = source_to_target_file == 0 || source_to_target_rank == 0 ||
        std::abs(source_to_target_file) == std::abs(source_to_target_rank);
    const bool target_king_line = target_to_king_file == 0 || target_to_king_rank == 0 ||
        std::abs(target_to_king_file) == std::abs(target_to_king_rank);
    return source_target_line && target_king_line &&
        sign(source_to_target_file) == sign(target_to_king_file) &&
        sign(source_to_target_rank) == sign(target_to_king_rank) &&
        (source_to_target_file != 0 || source_to_target_rank != 0) &&
        (target_to_king_file != 0 || target_to_king_rank != 0);
}

bool is_slider_on_line(koi::PieceType piece, koi::Square source, koi::Square target) {
    const int file_delta = std::abs((target.file() - 'a') - (source.file() - 'a'));
    const int rank_delta = std::abs((target.rank() - '1') - (source.rank() - '1'));
    const bool orthogonal = file_delta == 0 || rank_delta == 0;
    const bool diagonal = file_delta == rank_delta;
    return (piece == koi::PieceType::rook && orthogonal) ||
        (piece == koi::PieceType::bishop && diagonal) ||
        (piece == koi::PieceType::queen && (orthogonal || diagonal));
}

bool has_advanced_pawn(const koi::GameState& state, koi::Color color) {
    for (std::uint8_t index = 0; index < 64; ++index) {
        const koi::Square square = koi::Square::from_index(index);
        const koi::Piece piece = state.piece_at(square);
        if (piece.color != color || piece.type != koi::PieceType::pawn) {
            continue;
        }
        if ((color == koi::Color::white && square.rank() >= '5') ||
            (color == koi::Color::black && square.rank() <= '4')) {
            return true;
        }
    }
    return false;
}

struct LegacyExpectation {
    std::string_view name;
    std::string_view fen;
    std::string_view expected_move;
    std::uint8_t depth;
};

constexpr std::array<LegacyExpectation, 7> kLegacyExpectations{{
    {"mate_in_one", "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1", "f7e8", 2},
    {"queen_capture", "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1", "e4d5", 2},
    {"promotion", "4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8q", 2},
    {"fork", "8/2k1q3/8/8/8/2N5/8/K7 w - - 0 1", "c3d5", 3},
    {"pinned_queen", "4k3/4q3/8/8/1B6/8/8/K3R3 w - - 0 1", "e1e7", 2},
    {"recapture", "4k3/8/8/8/3p4/4P3/8/4K3 w - - 0 1", "e3d4", 2},
    {"black_mate", "8/8/8/8/8/1k6/2q5/K7 b - - 0 1", "c2a2", 2},
}};

const koi::StrengthPosition* find_fixture(std::span<const koi::StrengthPosition> positions,
                                          std::string_view name) {
    const auto found = std::find_if(positions.begin(), positions.end(),
                                    [name](const koi::StrengthPosition& position) {
                                        return position.name == name;
                                    });
    return found == positions.end() ? nullptr : &*found;
}

void validate_fixture_contract(std::span<const koi::StrengthPosition> positions,
                               std::size_t expected_count, bool hard_gate) {
    require(positions.size() == expected_count, "strength fixture corpus has an unexpected size");
    std::set<std::uint16_t> ids;
    std::set<std::string_view> names;
    std::set<std::string_view> fens;
    std::set<std::string_view> categories;
    std::map<std::string_view, std::size_t> category_counts;
    std::map<std::string_view, std::set<std::array<unsigned, 12>>> category_materials;
    std::map<std::string_view, std::set<std::string_view>> category_expected_moves;
    bool has_queen_capture = false;
    bool has_black_mate = false;
    std::size_t multi_solution_count = 0;

    for (const koi::StrengthPosition& position : positions) {
        require(position.id != 0 && !position.name.empty() && !position.category.empty(),
                "every fixture must carry an ID, name, and category");
        ids.insert(position.id);
        names.insert(position.name);
        fens.insert(position.fen);
        categories.insert(position.category);
        ++category_counts[position.category];
        has_queen_capture = has_queen_capture || position.name == "queen_capture";
        has_black_mate = has_black_mate || position.name == "black_mate";

        const auto state = koi::GameState::from_fen(position.fen);
        if (!state.has_value()) {
            throw std::runtime_error("invalid FEN in " + std::string(position.name));
        }
        category_materials[position.category].insert(material_signature(*state));
        category_expected_moves[position.category].insert(position.expected_move);
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
            if (!move.has_value() || !state->is_legal(*move)) {
                throw std::runtime_error("illegal accepted move in " + std::string(position.name) + ": " +
                                         std::string(move_text));
            }
            const auto metadata = state->describe_move(*move);
            require(metadata.has_value(), "every accepted move must have move metadata");
            if (position.category == "check") {
                require(metadata->gives_check, "check fixtures must accept only checking moves");
            }
            if (position.category == "mate") {
                koi::GameState after = *state;
                require(after.make_move(*move) && after.in_check() && after.legal_moves().empty(),
                        "mate fixtures must accept only checkmating moves: " +
                            std::string(position.name) + " " + std::string(move_text));
            }
            if (position.category == "promotion") {
                require(move->promotion() != koi::Promotion::none,
                        "promotion fixtures must accept a promotion move");
                require(metadata->moving_piece == koi::PieceType::pawn &&
                            metadata->kind == koi::MoveKind::promotion,
                        "promotion fixtures must promote a pawn");
            }
            if (position.category == "poisoned_capture") {
                require(metadata->is_capture() && metadata->captured_piece != koi::PieceType::none,
                        "every poisoned-capture allowlist move must capture a piece");
                require(is_attacked(state->position_features(), koi::opposite(state->side_to_move()),
                                    move->to()),
                        "poisoned captures must take a defended piece");
            }
            if (position.category == "queen_capture") {
                require(metadata->is_capture() && metadata->captured_piece == koi::PieceType::queen,
                        "queen_capture must capture a queen");
            }
            if (position.category == "evasion") {
                require(state->in_check(), "evasion fixtures must begin with the side to move in check");
                koi::GameState after = *state;
                require(after.make_move(*move) && !after.in_check(state->side_to_move()),
                        "evasion allowlist moves must resolve the check");
            }
            if (position.category == "fork") {
                koi::GameState after = *state;
                require(after.make_move(*move), "fork allowlist move must be applicable");
                const auto features = after.position_features();
                std::size_t attacked_targets = 0;
                for (std::uint8_t index = 0; index < 64; ++index) {
                    const koi::Piece piece = after.piece_at(koi::Square::from_index(index));
                    if (piece.color == koi::opposite(state->side_to_move()) &&
                        piece.type != koi::PieceType::none && piece.type != koi::PieceType::king &&
                        piece.type != koi::PieceType::pawn &&
                        ((features.attacked_squares[color_index(state->side_to_move())] >> index) & 1ULL) != 0) {
                        ++attacked_targets;
                    }
                }
                // The original public "fork" fixture predates the semantic
                // category checks and must remain source/data compatible even
                // though its historical position contains only one non-pawn
                // target. All newly added fork fixtures remain strict.
                if (position.name != "fork" && attacked_targets < 2) {
                    throw std::runtime_error("fork fixture has fewer than two non-pawn targets in " +
                                             std::string(position.name));
                }
            }
            if (position.category == "pin") {
                require(metadata->is_capture() && metadata->captured_piece != koi::PieceType::none,
                        "pin fixtures must capture the pinned piece");
                require(is_slider_on_line(metadata->moving_piece, move->from(), move->to()),
                        "pin fixtures must use a sliding piece on the pin line");
                const auto enemy_king = state->position_features().king_squares[
                    color_index(koi::opposite(state->side_to_move()))];
                require(lies_between_on_line(move->from(), move->to(), enemy_king),
                        "pin fixtures must capture a piece between a slider and the enemy king");
            }
            if (position.category == "defense") {
                require(metadata->is_capture() && metadata->captured_piece != koi::PieceType::none &&
                            (position.name == "recapture" ||
                             is_attacked(state->position_features(), state->side_to_move(), move->to())),
                        "defense fixtures must contain defensive recaptures");
            }
            if (position.category == "pawn_race") {
                require(metadata->moving_piece == koi::PieceType::pawn,
                        "pawn-race fixtures must move a pawn");
                require(has_advanced_pawn(*state, state->side_to_move()) &&
                            has_advanced_pawn(*state, koi::opposite(state->side_to_move())),
                        "pawn-race fixtures must contain advanced pawns for both sides");
                const int promotion_rank = state->side_to_move() == koi::Color::white ? '8' : '1';
                const int from_distance = std::abs(promotion_rank - static_cast<int>(move->from().rank()));
                const int to_distance = std::abs(promotion_rank - static_cast<int>(move->to().rank()));
                require(to_distance < from_distance,
                        "pawn-race fixtures must advance the moving pawn toward promotion");
            }
        }
        require(!accepted.empty(), "every fixture must carry an explicit accepted-move allowlist");
        multi_solution_count += accepted.size() > 1 ? 1U : 0U;
        require(accepted.size() == std::count_if(position.accepted_moves.begin(), position.accepted_moves.end(),
                                                  [](std::string_view move) { return !move.empty(); }),
                "accepted-move allowlists must not contain duplicate moves");
    }

    require(ids.size() == positions.size(), "fixture IDs must be unique");
    require(names.size() == positions.size(), "fixture names must be unique");
    require(fens.size() == positions.size(), "fixture FENs must be genuinely distinct");
    require(!hard_gate || multi_solution_count > 0,
            "hard gate must exercise at least one multi-solution allowlist");
    if (hard_gate) {
        for (const std::string_view category : {"mate", "check", "evasion", "fork", "pin",
                                                 "poisoned_capture", "promotion", "defense", "pawn_race"}) {
            require(categories.contains(category), "hard gate is missing a required tactical category");
        }
        require(category_counts["mate"] == 7 && category_counts["check"] == 7 &&
                    category_counts["evasion"] == 7 && category_counts["fork"] == 7 &&
                    category_counts["pin"] == 7 && category_counts["poisoned_capture"] == 7 &&
                    category_counts["promotion"] == 7 && category_counts["defense"] == 7 &&
                    category_counts["pawn_race"] == 7 && category_counts["queen_capture"] == 1,
                "hard gate category counts must retain seven curated cases per tactical category");
        require(has_queen_capture && has_black_mate,
                "hard gate must retain the legacy queen_capture and black_mate fixtures");
        for (const std::string_view category : {"mate", "check", "evasion", "fork", "pin",
                                                 "poisoned_capture", "promotion", "defense", "pawn_race"}) {
            if (category_materials[category].size() < 5) {
                throw std::runtime_error("hard category lacks material variation: " +
                                         std::string(category));
            }
            if (category_expected_moves[category].size() < 4) {
                throw std::runtime_error("hard category lacks expected-move variation: " +
                                         std::string(category));
            }
        }
    } else {
        for (const std::string_view category : {"positional", "king_safety", "endgame"}) {
            require(categories.contains(category), "optional corpus is missing a required category");
        }
        require(category_counts["positional"] == 43 && category_counts["king_safety"] == 43 &&
                    category_counts["endgame"] == 42,
                "optional corpus category counts must cover the three intended themes");
        for (const std::string_view category : {"positional", "king_safety", "endgame"}) {
            require(category_materials[category].size() >= 12,
                    "optional categories must contain materially varied positions");
            require(category_expected_moves[category].size() >= 12,
                    "optional categories must contain varied expected moves");
        }
    }
}

void test_strength_position_preserves_legacy_and_id_first_initialization() {
    const koi::StrengthPosition legacy{
        "compatibility", "4k3/8/8/8/8/8/8/4K3 w - - 0 1", "e1e2", 1};
    require(legacy.name == "compatibility" && legacy.fen == "4k3/8/8/8/8/8/8/4K3 w - - 0 1" &&
                legacy.expected_move == "e1e2" && legacy.depth == 1 && legacy.id == 0 &&
                legacy.accepted_moves[0].empty() && legacy.category.empty(),
            "the legacy four-field StrengthPosition initialization must remain source-compatible");

    const koi::StrengthPosition identified{
        42, "identified", "4k3/8/8/8/8/8/8/4K3 w - - 0 1", "e1e2", 1,
        {"e1e2", {}, {}}, "compatibility", std::optional<int>{17}};
    require(identified.id == 42 && identified.name == "identified" &&
                identified.accepted_moves[0] == "e1e2" && identified.category == "compatibility" &&
                identified.minimum_score == std::optional<int>{17},
            "the full ID-first StrengthPosition initialization must remain source-compatible");
}

void test_legacy_strength_rows_are_exact() {
    const auto positions = koi::strength_positions();
    for (const LegacyExpectation& expected : kLegacyExpectations) {
        const koi::StrengthPosition* actual = find_fixture(positions, expected.name);
        require(actual != nullptr, "hard gate is missing an exact legacy fixture");
        require(actual->fen == expected.fen && actual->expected_move == expected.expected_move &&
                    actual->depth == expected.depth,
                "legacy fixture FEN, expected move, and depth must remain exact");
    }
}

void test_strength_positions_are_valid_and_tactical_moves_are_found() {
    auto evaluator = std::make_shared<koi::ClassicalEvaluator>();
    koi::SearchService service(evaluator);
    for (const koi::StrengthPosition& position : koi::strength_positions()) {
        service.clear_hash();
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

void test_optional_strength_positions_are_not_rule_draws() {
    for (const koi::StrengthPosition& position : koi::optional_strength_positions()) {
        const auto state = koi::GameState::from_fen(position.fen);
        require(state.has_value(), "optional strength fixture must contain a valid FEN");
        require(!state->is_draw_by_rule(),
                "optional strength fixture must not begin as a rule draw: " +
                    std::string(position.name));
    }
}

void test_strength_corpora_are_cross_distinct_and_standard_legal() {
    std::set<std::string_view> fens;
    const auto is_historical_legacy = [](std::string_view name) {
        return name == "mate_in_one" || name == "queen_capture" || name == "promotion" ||
            name == "fork" || name == "pinned_queen" || name == "recapture" || name == "black_mate";
    };

    const auto inspect = [&fens, &is_historical_legacy](std::span<const koi::StrengthPosition> positions) {
        for (const koi::StrengthPosition& position : positions) {
            require(fens.insert(position.fen).second,
                    "hard and optional strength corpora must not reuse a FEN");
            const auto state = koi::GameState::from_fen(position.fen);
            require(state.has_value(), "strength corpus FEN must parse before legality inspection");
            if (!is_historical_legacy(position.name)) {
                require(!state->in_check(koi::opposite(state->side_to_move())),
                        "new strength fixture leaves the non-moving king in check: " +
                            std::string(position.name));
            }
        }
    };

    inspect(koi::strength_positions());
    inspect(koi::optional_strength_positions());
}

} // namespace

int main() {
    try {
        test_strength_suite_has_the_required_hard_gate_coverage();
        test_optional_strength_corpus_has_required_categories_and_metadata();
        test_optional_strength_positions_are_not_rule_draws();
        test_strength_corpora_are_cross_distinct_and_standard_legal();
        test_strength_position_preserves_legacy_and_id_first_initialization();
        test_legacy_strength_rows_are_exact();
        test_strength_positions_are_valid_and_tactical_moves_are_found();
        std::cout << "PASS strength suite\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL strength suite: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
