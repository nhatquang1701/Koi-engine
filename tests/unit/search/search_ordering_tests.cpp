#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/search_ordering.hpp"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "test FEN must construct a game state");
    return *state;
}

koi::Move require_move(std::string_view uci) {
    const auto move = koi::Move::parse_uci(uci);
    require(move.has_value(), "test move must parse");
    return *move;
}

std::vector<koi::Move> quiet_moves(const koi::GameState& state, std::vector<koi::Move> moves) {
    moves.erase(std::remove_if(moves.begin(), moves.end(), [&state](const koi::Move& move) {
        return state.is_capture(move) || move.promotion() != koi::Promotion::none;
    }), moves.end());
    return moves;
}

void test_tt_move_and_mvv_lva_capture_preference() {
    const koi::GameState state = require_state("4k3/8/8/3qp3/2P5/8/4Q3/4K3 w - - 0 1");
    const koi::Move pawn_takes_queen = require_move("c4d5");
    const koi::Move queen_takes_pawn = require_move("e2e5");
    koi::detail::SearchMoveOrdering ordering;

    std::vector<koi::Move> captures = state.legal_moves();
    ordering.order(state, captures, std::nullopt, 0);
    const auto pawn_queen = std::find(captures.begin(), captures.end(), pawn_takes_queen);
    const auto queen_pawn = std::find(captures.begin(), captures.end(), queen_takes_pawn);
    require(pawn_queen < queen_pawn,
            "MVV-LVA must prefer a pawn capturing a queen before a queen capturing a pawn");

    std::vector<koi::Move> tt_moves = state.legal_moves();
    ordering.order(state, tt_moves, queen_takes_pawn, 0);
    require(!tt_moves.empty() && tt_moves.front() == queen_takes_pawn,
            "a legal TT best move must outrank every tactical move");
}

void test_static_exchange_orders_safe_captures_ahead_of_poisoned_captures() {
    const koi::GameState state = require_state("3qk3/8/8/3r1p2/4Q1P1/8/8/4K3 w - - 0 1");
    const koi::Move poisoned_queen_capture = require_move("e4d5");
    const koi::Move safe_pawn_capture = require_move("g4f5");
    koi::detail::SearchMoveOrdering ordering;

    std::vector<koi::Move> captures = state.legal_moves();
    ordering.order(state, captures, std::nullopt, 0);
    const auto poisoned = std::find(captures.begin(), captures.end(), poisoned_queen_capture);
    const auto safe = std::find(captures.begin(), captures.end(), safe_pawn_capture);
    require(poisoned != captures.end() && safe != captures.end(),
            "the ordering fixture must retain both legal captures");
    require(safe < poisoned,
            "a free capture must be ordered ahead of a materially losing capture");
}

void test_killer_history_and_tie_breaking_are_deterministic() {
    const koi::GameState state = koi::GameState::startpos();
    const koi::Move killer = require_move("g1f3");
    const koi::Move history = require_move("b1c3");
    koi::detail::SearchMoveOrdering ordering;

    ordering.record_quiet_cutoff(state.side_to_move(), killer, 0, 4);
    std::vector<koi::Move> killer_moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, killer_moves, std::nullopt, 0);
    require(!killer_moves.empty() && killer_moves.front() == killer,
            "the current-ply killer must be the first quiet move");

    ordering.clear();
    ordering.record_quiet_cutoff(state.side_to_move(), history, 0, 6);
    std::vector<koi::Move> history_moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, history_moves, std::nullopt, 1);
    require(!history_moves.empty() && history_moves.front() == history,
            "history must prefer a previously successful quiet move when no current-ply killer exists");

    ordering.clear();
    std::vector<koi::Move> first = state.legal_moves();
    std::vector<koi::Move> second = state.legal_moves();
    ordering.order(state, first, std::nullopt, 0);
    ordering.order(state, second, std::nullopt, 0);
    require(first == second, "equal-priority moves must have a stable deterministic order");
}

void test_killer_tier_outranks_saturated_history() {
    const koi::GameState state = koi::GameState::startpos();
    const koi::Move killer = require_move("g1f3");
    const koi::Move history = require_move("b1c3");
    koi::detail::SearchMoveOrdering ordering;

    ordering.record_quiet_cutoff(state.side_to_move(), killer, 0, 4);
    for (int count = 0; count < 100; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 1, 64);
    }

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 0);
    require(!moves.empty() && moves.front() == killer,
            "a killer must outrank even a repeatedly reinforced history move");
}

void test_countermove_tier_outranks_history_but_not_a_killer() {
    koi::GameState state = koi::GameState::startpos();
    const koi::Move previous = require_move("e2e4");
    const koi::Move counter = require_move("g8f6");
    const koi::Move history = require_move("b8c6");
    const koi::Move killer = require_move("e7e5");
    require(state.make_move(previous), "the counter-move fixture must apply the previous move");
    require(state.is_legal(counter) && state.is_legal(history) && state.is_legal(killer),
            "the counter-move fixture must retain three legal quiet replies");

    koi::detail::SearchMoveOrdering ordering;
    for (int count = 0; count < 100; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 4, 64);
    }
    ordering.record_quiet_cutoff(state.side_to_move(), counter, 4, 8, previous);
    ordering.record_quiet_cutoff(state.side_to_move(), killer, 3, 4);

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 3, previous);
    const auto killer_position = std::find(moves.begin(), moves.end(), killer);
    const auto counter_position = std::find(moves.begin(), moves.end(), counter);
    const auto history_position = std::find(moves.begin(), moves.end(), history);
    require(killer_position != moves.end() && counter_position != moves.end() &&
                history_position != moves.end(),
            "the ordered fixture must retain the killer, counter move, and history move");
    require(killer_position < counter_position && counter_position < history_position,
            "a counter move must outrank saturated quiet history while remaining below a killer");
}

void test_unproven_countermove_does_not_outrank_saturated_history() {
    koi::GameState state = koi::GameState::startpos();
    const koi::Move previous = require_move("e2e4");
    const koi::Move counter = require_move("g8f6");
    const koi::Move history = require_move("b8c6");
    require(state.make_move(previous), "the unproven counter fixture must apply the previous move");
    require(state.is_legal(counter) && state.is_legal(history),
            "the unproven counter fixture must retain two legal quiet replies");

    koi::detail::SearchMoveOrdering ordering;
    for (int count = 0; count < 100; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 4, 64);
    }
    ordering.record_quiet_cutoff(state.side_to_move(), counter, 4, 6, previous);

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 3, previous);
    const auto counter_position = std::find(moves.begin(), moves.end(), counter);
    const auto history_position = std::find(moves.begin(), moves.end(), history);
    require(counter_position != moves.end() && history_position != moves.end(),
            "the ordered fixture must retain the unproven counter and history move");
    require(history_position < counter_position,
            "one shallow counter cutoff must not outrank saturated quiet history");
}

void test_countermove_confidence_ignores_colliding_continuation_history() {
    koi::GameState state = koi::GameState::startpos();
    const koi::Move previous = require_move("e2e4");
    const koi::Move counter = require_move("g8f6");
    const koi::Move history = require_move("b8c6");
    // These geometrically legal coordinate pairs share the target continuation-history slot.
    const koi::Move colliding_previous = require_move("a1f1");
    const koi::Move colliding_reply = require_move("e7c5");
    require(state.make_move(previous), "the collision fixture must apply the previous move");
    require(state.is_legal(counter) && state.is_legal(history),
            "the collision fixture must retain two legal quiet replies");

    koi::detail::SearchMoveOrdering ordering;
    for (int count = 0; count < 500; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 4, 64);
    }
    for (int count = 0; count < 100; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), colliding_reply, 4, 64,
                                     colliding_previous);
    }
    ordering.record_quiet_cutoff(state.side_to_move(), counter, 4, 6, previous);
    require(ordering.quiet_history_score(state.side_to_move(), counter, previous) >= 128,
            "the collision fixture must raise the target combined history above the counter gate");

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 3, previous);
    const auto counter_position = std::find(moves.begin(), moves.end(), counter);
    const auto history_position = std::find(moves.begin(), moves.end(), history);
    require(counter_position != moves.end() && history_position != moves.end(),
            "the collision fixture must retain the counter and history move");
    require(history_position < counter_position,
            "a shallow counter must not claim counter priority from colliding continuation history");
}

void test_countermove_failure_clears_exact_confidence() {
    koi::GameState state = koi::GameState::startpos();
    const koi::Move previous = require_move("e2e4");
    const koi::Move counter = require_move("g8f6");
    const koi::Move history = require_move("b8c6");
    require(state.make_move(previous), "the counter-failure fixture must apply the previous move");
    require(state.is_legal(counter) && state.is_legal(history),
            "the counter-failure fixture must retain two legal quiet replies");

    koi::detail::SearchMoveOrdering ordering;
    for (int count = 0; count < 100; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 4, 64);
    }
    ordering.record_quiet_cutoff(state.side_to_move(), counter, 4, 8, previous);

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 3, previous);
    require(std::find(moves.begin(), moves.end(), counter) <
                std::find(moves.begin(), moves.end(), history),
            "a proven counter must receive the special counter tier before it fails");

    ordering.record_quiet_fail(state.side_to_move(), counter, 4, 8, previous);
    moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 3, previous);
    require(std::find(moves.begin(), moves.end(), history) <
                std::find(moves.begin(), moves.end(), counter),
            "a failed exact counter must lose special counter priority");
}

void test_quiet_checks_are_ordered_before_ordinary_quiet_moves() {
    const koi::GameState state = require_state("k7/8/8/8/8/8/1q2Q3/4K3 w - - 0 1");
    koi::detail::SearchMoveOrdering ordering;

    std::vector<koi::Move> moves = state.legal_moves();
    ordering.order(state, moves, std::nullopt, 0);
    const koi::Move capture = require_move("e2b2");
    const auto checking = std::find_if(moves.begin(), moves.end(), [&state](const koi::Move& move) {
        const auto metadata = state.describe_move(move);
        return metadata.has_value() && metadata->gives_check && !metadata->is_capture();
    });
    const auto ordinary = std::find_if(moves.begin(), moves.end(), [&state](const koi::Move& move) {
        const auto metadata = state.describe_move(move);
        return metadata.has_value() && !metadata->gives_check && !metadata->is_capture() &&
            move.promotion() == koi::Promotion::none;
    });
    const auto captured = std::find(moves.begin(), moves.end(), capture);
    require(checking != moves.end() && ordinary != moves.end() && captured != moves.end(),
            "the ordering fixture must contain a quiet check, ordinary quiet move, and capture");
    require(captured < checking,
            "a higher-priority capture must remain ahead of a quiet checking move");
    require(checking < ordinary,
            "a quiet checking move must be searched before ordinary quiet moves");
}

void test_history_malus_and_continuation_history_shape_quiet_ordering() {
    const koi::GameState state = koi::GameState::startpos();
    const koi::Move prior = require_move("e2e4");
    const koi::Move preferred = require_move("b1c3");
    const koi::Move penalized = require_move("g1f3");
    koi::detail::SearchMoveOrdering ordering;

    ordering.record_quiet_cutoff(state.side_to_move(), preferred, 0, 6, prior);
    for (int count = 0; count < 8; ++count) {
        ordering.record_quiet_fail(state.side_to_move(), penalized, 0, 6, prior);
    }

    std::vector<koi::Move> moves = quiet_moves(state, state.legal_moves());
    ordering.order(state, moves, std::nullopt, 1, prior);
    const auto preferred_position = std::find(moves.begin(), moves.end(), preferred);
    const auto penalized_position = std::find(moves.begin(), moves.end(), penalized);
    require(preferred_position < penalized_position,
            "continuation history bonus and quiet history malus must shape quiet ordering");
}

void test_history_saturation_does_not_overflow_signed_intermediates() {
    const koi::GameState state = koi::GameState::startpos();
    const koi::Move prior = require_move("e2e4");
    const koi::Move move = require_move("b1c3");
    koi::detail::SearchMoveOrdering ordering;

    for (int count = 0; count < 500; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), move, 0, 64, prior);
    }

    const int score = ordering.quiet_history_score(state.side_to_move(), move, prior);
    require(score >= 0 && score <= 299'999,
            "saturated quiet and continuation history must remain within signed bounds");
}

void test_metadata_ordering_score_is_cached_with_see() {
    const koi::GameState state = require_state("4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves);
    koi::detail::SearchMoveOrdering ordering;
    ordering.order(state, moves, std::nullopt, 0);

    const auto selected = std::find_if(moves.begin(), moves.end(), [](const koi::MoveMetadata& metadata) {
        return metadata.move == *koi::Move::parse_uci("e4d5");
    });
    require(selected != moves.end(), "ordering cache fixture must include the queen capture");
    require(selected->see_score >= 500,
            "ordering must preserve the cached SEE score from move generation");
    require(selected->ordering_score > 0,
            "ordering must publish its computed score into move metadata");
}

void test_search_move_generation_can_defer_see() {
    const koi::GameState state = require_state(
        "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves, true, false);
    const auto selected = std::find_if(moves.begin(), moves.end(), [](const koi::MoveMetadata& metadata) {
        return metadata.move == *koi::Move::parse_uci("e4d5");
    });
    require(selected != moves.end(), "deferred SEE fixture must include the queen capture");
    require(!selected->see_computed,
            "normal search move generation must be able to defer expensive SEE calculation");
}

void test_search_move_generation_can_skip_capture_check_analysis() {
    const koi::GameState state = require_state(
        "4k3/4p3/8/8/8/8/8/4R1K1 w - - 0 1");
    const auto checking_capture = koi::Move::parse_uci("e1e7");
    require(checking_capture.has_value(), "checking capture fixture move must parse");

    koi::MoveMetadataList complete;
    state.legal_moves_with_metadata(complete, true, false);
    const auto complete_move = std::find_if(complete.begin(), complete.end(),
        [&checking_capture](const koi::MoveMetadata& metadata) {
            return metadata.move == *checking_capture;
        });
    require(complete_move != complete.end() && complete_move->gives_check,
            "the complete metadata path must identify a checking capture");

    koi::MoveMetadataList search_metadata;
    state.legal_moves_with_metadata(search_metadata, true, false,
                                    koi::CheckFlagMode::quiet_moves_only);
    const auto search_move = std::find_if(search_metadata.begin(), search_metadata.end(),
        [&checking_capture](const koi::MoveMetadata& metadata) {
            return metadata.move == *checking_capture;
        });
    require(search_move != search_metadata.end() && !search_move->gives_check,
            "the search metadata path must skip check analysis for captures");
}

void test_ordering_resolves_deferred_see_for_search_moves() {
    const koi::GameState state = require_state(
        "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1");
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves, true, false);
    const auto selected_before = std::find_if(moves.begin(), moves.end(), [](const koi::MoveMetadata& metadata) {
        return metadata.move == *koi::Move::parse_uci("e4d5");
    });
    require(selected_before != moves.end() && !selected_before->see_computed,
            "the normal search fixture must start with deferred SEE");

    koi::detail::SearchMoveOrdering ordering;
    ordering.order(state, moves, std::nullopt, 0);
    const auto selected_after = std::find_if(moves.begin(), moves.end(), [](const koi::MoveMetadata& metadata) {
        return metadata.move == *koi::Move::parse_uci("e4d5");
    });
    require(selected_after != moves.end() && selected_after->see_computed &&
                selected_after->see_score >= 500,
            "ordering must resolve deferred SEE before ranking a capture");
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"TT and MVV-LVA ordering", test_tt_move_and_mvv_lva_capture_preference},
        {"SEE capture ordering", test_static_exchange_orders_safe_captures_ahead_of_poisoned_captures},
        {"killer history stable ordering", test_killer_history_and_tie_breaking_are_deterministic},
        {"killer tier outranks saturated history", test_killer_tier_outranks_saturated_history},
        {"counter move tier", test_countermove_tier_outranks_history_but_not_a_killer},
        {"counter move confidence", test_unproven_countermove_does_not_outrank_saturated_history},
        {"counter move collision confidence", test_countermove_confidence_ignores_colliding_continuation_history},
        {"counter move failure confidence", test_countermove_failure_clears_exact_confidence},
        {"quiet checks before quiet moves", test_quiet_checks_are_ordered_before_ordinary_quiet_moves},
        {"history malus and continuation ordering", test_history_malus_and_continuation_history_shape_quiet_ordering},
        {"history saturation overflow safety", test_history_saturation_does_not_overflow_signed_intermediates},
        {"cached move scores", test_metadata_ordering_score_is_cached_with_see},
        {"deferred SEE generation", test_search_move_generation_can_defer_see},
        {"quiet-only check metadata", test_search_move_generation_can_skip_capture_check_analysis},
        {"deferred SEE ordering", test_ordering_resolves_deferred_see_for_search_moves},
    };

    for (const TestCase& test : tests) {
        try {
            test.run();
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
