#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/search_ordering.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

koi::GameState require_state(std::string_view fen) {
    return koi::test::require_value(koi::GameState::from_fen(fen),
                                    "test FEN must construct a game state");
}

koi::Move require_move(std::string_view uci) {
    return koi::test::require_value(koi::Move::parse_uci(uci), "test move must parse");
}

// SearchHistoryContext is a write-before-read type with no default member
// initializers; this reproduces the value-initialized context the tests used
// to construct with `{}`.
koi::detail::SearchHistoryContext zeroed_history_context() {
    koi::detail::SearchHistoryContext context;
    context.continuation_moves.fill(koi::Move::no_move());
    context.continuation_previous_scaled.fill(0);
    context.count = 0;
    context.ply = 0;
    context.pawn_key = 0;
    return context;
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
    // Keep the fixture below the 16,384 fixed-point history clamp so the two
    // moves stay distinguishable by their accumulated history instead of both
    // saturating at the ceiling (the fixture predates that shared scale).
    for (int count = 0; count < 20; ++count) {
        ordering.record_quiet_cutoff(state.side_to_move(), history, 4, 64);
    }
    for (int count = 0; count < 2; ++count) {
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

void test_parent_move_feedback_rewards_fail_low_and_penalizes_refutation() {
    koi::detail::SearchOrderingTables tables;
    const koi::Color side = koi::Color::white;
    const koi::Move parent = require_move("g1f3");
    const koi::Move grandparent = require_move("b1c3");
    const koi::Move great_grandparent = require_move("d2d4");

    koi::detail::SearchHistoryContext child_context = zeroed_history_context();
    child_context.ply = 3;
    child_context.count = 3;
    child_context.continuation_moves[0] = parent;
    child_context.continuation_moves[1] = grandparent;
    child_context.continuation_moves[2] = great_grandparent;

    // The same tables that order the parent move at its own node must observe
    // the reward: distance-zero continuation pair plus the multi-ply slot.
    koi::detail::SearchHistoryContext parent_context = zeroed_history_context();
    parent_context.ply = 2;
    parent_context.count = 2;
    parent_context.continuation_moves[0] = grandparent;
    parent_context.continuation_moves[1] = great_grandparent;
    koi::MoveMetadata parent_metadata{};
    parent_metadata.move = parent;
    parent_metadata.moving_piece = koi::PieceType::knight;

    const int baseline = tables.quiet_history_score(side, parent);
    const int baseline_context =
        tables.quiet_history_score(side, parent_metadata, parent_context);
    tables.record_parent_fail_low(side, parent, koi::PieceType::knight, 2, child_context, 8);
    const int rewarded = tables.quiet_history_score(side, parent);
    const int rewarded_context =
        tables.quiet_history_score(side, parent_metadata, parent_context);
    require(rewarded > baseline, "a parent fail low must reward the move that caused it");
    require(rewarded_context > baseline_context,
            "parent feedback must update the continuation history consulted at its own node");

    tables.record_parent_refuted(side, parent, koi::PieceType::knight, 2, child_context, 8);
    require(tables.quiet_history_score(side, parent_metadata, parent_context) < rewarded_context,
            "a refuted parent move must lose continuation history");
    require(tables.quiet_history_score(side, parent) == rewarded,
            "refutation feedback must leave the parent's main history untouched");

    // Promotions are not quiet-history material.
    const koi::Move promotion = require_move("e7e8q");
    const int promotion_before = tables.quiet_history_score(side, promotion);
    tables.record_parent_fail_low(side, promotion, koi::PieceType::queen, 2, child_context, 8);
    require(tables.quiet_history_score(side, promotion) == promotion_before,
            "parent feedback must ignore promotion moves");
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

void test_picker_emitted_scores_match_recomputed_priority() {
    // The picker stages each candidate with one stored priority and emits it
    // later.  Whenever the emitted metadata cannot differ from the staged
    // metadata, the emitted score must still equal a fresh computation with
    // the same ordering tables and history context.
    constexpr std::string_view fens[] = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "4k3/8/8/8/8/8/8/R3K3 w - - 0 1",
        "8/2b1k3/7p/p1p1p2P/PpP1P3/1P1BK3/8/8 w - - 0 1",
    };
    for (const std::string_view fen : fens) {
        const koi::GameState state = require_state(fen);
        koi::detail::SearchMoveOrdering ordering;
        const koi::detail::SearchHistoryContext context = zeroed_history_context();
        for (const auto mode : {koi::detail::SearchMovePicker::Mode::main,
                                koi::detail::SearchMovePicker::Mode::quiescence}) {
            koi::MoveMetadataList moves;
            if (mode == koi::detail::SearchMovePicker::Mode::quiescence) {
                static_cast<void>(
                    state.legal_tactical_moves_with_metadata(moves, true, false));
            } else {
                state.legal_moves_with_metadata(moves, true, false);
            }
            koi::detail::SearchMovePicker picker(ordering, state, moves, std::nullopt,
                                                 &context, 0, mode);
            while (const koi::MoveMetadata* emitted = picker.next()) {
                koi::MoveMetadataList single;
                static_cast<void>(single.push_back(*emitted));
                ordering.order(state, single, std::nullopt, context);
                require(single[0].ordering_score == emitted->ordering_score,
                        "emitted picker scores must match a fresh priority computation");
            }
        }
    }
}

// Independent reference for the quiescence picker's documented emission
// rule: candidates are grouped into the capture/promotion stage and the
// quiet-check stage, each stage is ordered by its packed ascending key
// (priority descending, move tie-break ascending), and non-checking quiet
// moves never enter a stage.  The picker must reproduce that sequence while
// emitting lazily.
struct QuiescenceReferenceEntry {
    koi::MoveMetadata metadata{};
    int priority = 0;
    std::uint32_t tie_break = 0;
    std::uint64_t sort_key = 0;
};

void verify_quiescence_picker_against_reference(
    const koi::GameState& state, const koi::detail::SearchMoveOrdering& ordering,
    const koi::detail::SearchHistoryContext& context,
    const koi::MoveMetadataList& candidates, int& reference_captures,
    int& reference_promotions, int& reference_checks, int& dropped_quiets) {
    std::vector<QuiescenceReferenceEntry> stages[2];
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const koi::MoveMetadata& source = candidates[index];
        const bool tactical = source.is_capture() ||
            source.move.promotion() != koi::Promotion::none;
        if (!tactical && !source.gives_check) {
            ++dropped_quiets;
            continue;
        }
        // The public single-candidate ordering path resolves deferred SEE
        // exactly like the picker's prepare pass ranks a tactical candidate
        // and publishes the priority in ordering_score, so it is the
        // priority oracle here.
        koi::MoveMetadataList single;
        require(single.push_back(source), "reference list must accept the candidate record");
        ordering.order(state, single, std::nullopt, context);
        const int priority = single[0].ordering_score;
        const std::uint32_t tie_break = koi::detail::move_tie_break_key(source.move);
        stages[tactical ? 0 : 1].push_back(QuiescenceReferenceEntry{
            single[0], priority, tie_break,
            (static_cast<std::uint64_t>(
                 ~(static_cast<std::uint32_t>(priority) ^ 0x80000000U)) << 32U) | tie_break});
        if (source.move.promotion() != koi::Promotion::none) {
            ++reference_promotions;
        }
    }
    for (std::vector<QuiescenceReferenceEntry>& stage : stages) {
        std::sort(stage.begin(), stage.end(),
                  [](const QuiescenceReferenceEntry& lhs,
                     const QuiescenceReferenceEntry& rhs) {
                      return lhs.sort_key < rhs.sort_key;
                  });
        for (std::size_t index = 1; index < stage.size(); ++index) {
            require(stage[index - 1].priority > stage[index].priority ||
                        (stage[index - 1].priority == stage[index].priority &&
                         stage[index - 1].tie_break < stage[index].tie_break),
                    "reference stage order must be priority-descending with an ascending tie-break");
        }
    }
    reference_captures += static_cast<int>(stages[0].size());
    reference_checks += static_cast<int>(stages[1].size());

    koi::detail::SearchMovePicker picker(
        ordering, state, candidates, std::nullopt, &context, 0,
        koi::detail::SearchMovePicker::Mode::quiescence);
    const auto verify_emitted = [&picker](const QuiescenceReferenceEntry& expected) {
        const koi::MoveMetadata* emitted = picker.next();
        require(emitted != nullptr,
                "quiescence picker must emit a record for every reference-stage candidate");
        require(emitted->move == expected.metadata.move,
                "quiescence picker emission order must match the reference stage order");
        require(emitted->moving_piece == expected.metadata.moving_piece &&
                    emitted->captured_piece == expected.metadata.captured_piece &&
                    emitted->kind == expected.metadata.kind &&
                    emitted->position_key == expected.metadata.position_key &&
                    emitted->validation_token == expected.metadata.validation_token,
                "quiescence picker must emit the staged source record unchanged");
        require(emitted->gives_check == expected.metadata.gives_check,
                "quiescence picker must preserve the staged check flag");
        require(emitted->see_computed == expected.metadata.see_computed &&
                    emitted->see_score == expected.metadata.see_score,
                "quiescence picker must preserve the resolved SEE side effects");
        require(emitted->ordering_score == expected.metadata.ordering_score,
                "quiescence picker must emit the staged ordering score");
    };
    for (const QuiescenceReferenceEntry& entry : stages[0]) {
        verify_emitted(entry);
    }
    for (const QuiescenceReferenceEntry& entry : stages[1]) {
        verify_emitted(entry);
    }
    require(picker.next() == nullptr,
            "quiescence picker must not emit candidates outside the two reachable stages");
}

void test_quiescence_picker_matches_documented_reference() {
    constexpr std::string_view fens[] = {
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "1n2k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "k7/8/8/8/8/8/1q2Q3/4K3 w - - 0 1",
        "4k3/4p3/8/8/8/8/8/4R1K1 w - - 0 1",
        "8/2b1k3/7p/p1p1p2P/PpP1P3/1P1BK3/8/8 w - - 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2",
    };
    int reference_captures = 0;
    int reference_promotions = 0;
    int reference_checks = 0;
    int dropped_quiets = 0;
    std::mt19937 random(0x4B4F4951U);
    for (const std::string_view fen : fens) {
        const koi::GameState state = require_state(fen);
        for (const bool include_see : {false, true}) {
            for (const bool tactical_only : {false, true}) {
                koi::MoveMetadataList generated;
                if (tactical_only) {
                    static_cast<void>(state.legal_tactical_moves_with_metadata(
                        generated, true, include_see));
                } else {
                    state.legal_moves_with_metadata(generated, true, include_see);
                }

                // Deterministic adaptive-history shape so the differential
                // exercises separated capture and quiet-check priorities:
                // every third tactical record gets capture feedback and every
                // third quiet record quiet feedback at the picker's ply.
                koi::detail::SearchMoveOrdering ordering;
                koi::detail::SearchHistoryContext context = zeroed_history_context();
                context.ply = 3;
                for (std::size_t index = 0; index < generated.size(); ++index) {
                    const koi::MoveMetadata& metadata = generated[index];
                    const bool tactical = metadata.is_capture() ||
                        metadata.move.promotion() != koi::Promotion::none;
                    if (index % 3 != 0) {
                        continue;
                    }
                    if (tactical) {
                        ordering.record_capture_best(metadata, 4, context);
                    } else {
                        ordering.record_quiet_cutoff(state.side_to_move(), metadata, 4, context);
                    }
                }

                // The generator's own order and a shuffled copy must emit
                // identically: the staged key order, not the input order,
                // fixes the sequence.
                koi::MoveMetadataList shuffled = generated;
                std::shuffle(shuffled.begin(), shuffled.end(), random);
                verify_quiescence_picker_against_reference(
                    state, ordering, context, generated, reference_captures,
                    reference_promotions, reference_checks, dropped_quiets);
                verify_quiescence_picker_against_reference(
                    state, ordering, context, shuffled, reference_captures,
                    reference_promotions, reference_checks, dropped_quiets);

                // A pruned candidate window releases only part of the list.
                koi::MoveMetadataList window = shuffled;
                window.resize(std::min<std::size_t>(3, window.size()));
                verify_quiescence_picker_against_reference(
                    state, ordering, context, window, reference_captures,
                    reference_promotions, reference_checks, dropped_quiets);
            }
        }
    }
    require(reference_captures > 0,
            "the differential fixtures must emit capture candidates");
    require(reference_promotions > 0,
            "the differential fixtures must emit promotion candidates");
    require(reference_checks > 0,
            "the differential fixtures must emit quiet-check candidates");
    require(dropped_quiets > 0,
            "the differential fixtures must drop non-checking quiet candidates");
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

void test_correction_history_saturates_and_resets() {
    koi::detail::SearchMoveOrdering ordering;
    constexpr std::uint64_t pawn_key = 0x0123456789ABCDEFULL;
    constexpr std::uint64_t material_key = 0xFEDCBA9876543210ULL;
    constexpr std::uint64_t king_key = 0x1111222233334444ULL;

    require(ordering.correction_value(pawn_key, material_key, king_key) == 0,
            "correction history must start neutral");

    for (int index = 0; index < 64; ++index) {
        ordering.update_correction(pawn_key, material_key, king_key, 48);
    }
    const int raised = ordering.correction_value(pawn_key, material_key, king_key);
    require(raised > 0, "positive correction updates must raise the correction");
    require(raised <= 192, "the summed correction must stay bounded");

    for (int index = 0; index < 128; ++index) {
        ordering.update_correction(pawn_key, material_key, king_key, -48);
    }
    const int lowered = ordering.correction_value(pawn_key, material_key, king_key);
    require(lowered < 0, "negative correction updates must lower the correction");
    require(lowered >= -192, "the summed correction must stay bounded in both directions");

    ordering.clear();
    require(ordering.correction_value(pawn_key, material_key, king_key) == 0,
            "clear must reset the correction tables");
}

void test_fresh_quiet_history_is_centred_on_zero() {
    // The LMR bands compare composite history against zero, so a quiet move
    // that has never been updated must read 0 (the negative table priors are
    // subtracted as the context baseline).
    const koi::GameState state = require_state("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    koi::detail::SearchMoveOrdering ordering;
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves);
    ordering.order(state, moves, std::nullopt, 0);
    for (const koi::MoveMetadata& metadata : moves) {
        if (metadata.is_capture() || metadata.gives_check ||
            metadata.move.promotion() != koi::Promotion::none) {
            continue;
        }
        require(metadata.ordering_score == 0,
                "an untrained quiet move must have a zero-centred history score");
    }
}

void test_quiet_history_gravity_keeps_the_lmr_scale() {
    // Regression for a 299,998-clamp bug: the main history table shared the
    // counter-move priority clamp instead of the fixed-point 16,384 scale used
    // by every other table and by the LMR bands.  Gravity is (less than)
    // linear in the clamp, so the wider clamp let repeated bonuses accumulate
    // into the thousands and excluded most good quiet moves from LMR.
    const koi::GameState state = require_state("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    koi::detail::SearchMoveOrdering ordering;
    const koi::Move move = require_move("e2e4");
    for (int repeat = 0; repeat < 400; ++repeat) {
        ordering.record_quiet_cutoff(state.side_to_move(), move, 0, 8);
    }
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves);
    ordering.order(state, moves, std::nullopt, 1);
    const auto selected = std::find_if(moves.begin(), moves.end(), [&move](const koi::MoveMetadata& metadata) {
        return metadata.move == move;
    });
    require(selected != moves.end(), "the history scale fixture must retain the quiet move");
    require(selected->ordering_score <= 16'384,
            "quiet history must saturate at the shared 16,384 fixed-point scale");
    require(selected->ordering_score > 0,
            "repeated quiet cutoffs must keep a positive history score");
    // 400 x bonus(8) is 25,600 before gravity; the aged score must stay below
    // the raw accumulator, proving the table actually decays.
    require(selected->ordering_score < 400 * 64,
            "quiet history gravity must age repeated bonuses instead of accumulating");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
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
        {"parent move feedback", test_parent_move_feedback_rewards_fail_low_and_penalizes_refutation},
        {"cached move scores", test_metadata_ordering_score_is_cached_with_see},
        {"picker emitted scores", test_picker_emitted_scores_match_recomputed_priority},
        {"quiescence picker differential", test_quiescence_picker_matches_documented_reference},
        {"deferred SEE generation", test_search_move_generation_can_defer_see},
        {"quiet-only check metadata", test_search_move_generation_can_skip_capture_check_analysis},
        {"deferred SEE ordering", test_ordering_resolves_deferred_see_for_search_moves},
        {"correction history saturation", test_correction_history_saturates_and_resets},
        {"quiet history gravity scale", test_quiet_history_gravity_keeps_the_lmr_scale},
        {"fresh quiet history centring", test_fresh_quiet_history_is_centred_on_zero},
    };

    return koi::test::run_tests(tests, argc, argv);
}
