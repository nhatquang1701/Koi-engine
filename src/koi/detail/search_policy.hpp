#pragma once

#include <algorithm>

#include "koi/detail/search_constants.hpp"

namespace koi::detail {

struct NullMoveDecision {
    bool eligible = false;
    int reduction = 0;
};

struct DynamicNullMoveDecision {
    bool eligible = false;
    int reduction = 0;
    bool verify = false;
};

struct LateMoveDecision {
    bool candidate = false;
    bool high_history_exclusion = false;
    int reduction = 0;
    bool reduced = false;
};

struct ProbCutDecision {
    bool eligible = false;
    int beta = 0;
    int depth = 0;
};

enum class QuiescenceCapturePrune {
    none,
    static_exchange,
    delta,
};

// SearchPolicy centralizes scalar pruning, reduction, and extension gates.
// It has no position access, mutable state, allocation, or statistics side effects.
class SearchPolicy {
public:
    [[nodiscard]] static constexpr NullMoveDecision null_move(
        const int depth, const int alpha, const int beta,
        const bool checked, const bool allowed) noexcept {
        const bool eligible = allowed && !checked && depth >= 3 &&
            beta < kInfinity && beta > -kInfinity && beta - alpha <= 1;
        return NullMoveDecision{eligible, eligible ? (depth >= 6 ? 3 : 2) : 0};
    }

    // The compatibility null_move() above intentionally retains its small,
    // stable contract. Live search uses this dynamic form, which follows the
    // Stockfish 19 shape: deeper nodes and positions comfortably above beta
    // can afford a larger null reduction, while marginal positions retain a
    // shallower probe and are more likely to be verified.
    [[nodiscard]] static constexpr DynamicNullMoveDecision dynamic_null_move(
        const int depth, const int alpha, const int beta, const int static_eval,
        const bool checked, const bool allowed, const bool improving,
        const bool pawn_endgame) noexcept {
        const NullMoveDecision base = null_move(depth, alpha, beta, checked, allowed);
        if (!base.eligible || pawn_endgame || static_eval < beta - 80 - depth * 24) {
            return {};
        }

        const int excess = std::max(0, static_eval - beta);
        const int reduction = std::clamp(
            2 + depth / 5 + std::min(2, excess / 192) + (!improving ? 1 : 0),
            2, std::max(2, depth - 2));
        return DynamicNullMoveDecision{true, reduction, depth >= 5 && reduction >= 3};
    }

    [[nodiscard]] static constexpr bool check_extension(
        const bool checked, const int depth, const bool short_timed_root,
        const int extensions_remaining) noexcept {
        return checked && depth < kMaximumSearchDepth && !short_timed_root &&
            extensions_remaining > 0;
    }

    [[nodiscard]] static constexpr LateMoveDecision late_move(
        const int depth, const int move_number, const int full_child_depth,
        const int history_score, const bool root_pawn_move, const bool checked,
        const bool gives_check, const bool capture, const bool promotion,
        const bool tt_move, const bool killer, const bool reducible_quiet,
        const bool quiet_forcing) noexcept {
        const bool candidate = !root_pawn_move && move_number >= 4 && depth >= 4 &&
            !checked && !gives_check && !capture && !promotion && !tt_move && !killer;
        const bool high_history_exclusion = candidate &&
            high_history_move_excluded_from_lmr(history_score);
        const int base_reduction = 1 + (depth >= 8 && move_number >= 12 ? 1 : 0) +
            (depth >= 12 && move_number >= 20 ? 1 : 0);
        const int history_adjustment = history_score > 256 ? -1 :
            history_score < -256 ? 1 : 0;
        const int reduction = std::clamp(
            base_reduction + history_adjustment, 0, std::max(0, full_child_depth));
        const bool reduced = candidate && !high_history_exclusion && reducible_quiet &&
            !quiet_forcing && reduction > 0;
        return LateMoveDecision{candidate, high_history_exclusion, reduction, reduced};
    }

    [[nodiscard]] static constexpr LateMoveDecision dynamic_late_move(
        const int depth, const int move_number, const int full_child_depth,
        const int history_score, const int continuation_score,
        const int capture_history_score, const bool root_pawn_move,
        const bool checked, const bool gives_check, const bool capture,
        const bool promotion, const bool tt_move, const bool killer,
        const bool reducible_quiet, const bool quiet_forcing,
        const bool pv_node, const bool cut_node, const bool improving,
        const bool prior_fail_high, const bool has_tt_move) noexcept {
        const bool candidate = !root_pawn_move && move_number >= 3 && depth >= 3 &&
            !checked && !gives_check && !capture && !promotion && !tt_move && !killer;
        const bool high_history_exclusion = candidate &&
            high_history_move_excluded_from_lmr(history_score);
        if (!candidate) {
            return LateMoveDecision{false, false, 0, false};
        }

        int reduction = 1;
        if (depth >= 7 && move_number >= 7) {
            ++reduction;
        }
        if (depth >= 11 && move_number >= 14) {
            ++reduction;
        }
        if (depth >= 16 && move_number >= 24) {
            ++reduction;
        }
        if (cut_node) {
            ++reduction;
        }
        if (!improving) {
            ++reduction;
        }
        if (pv_node) {
            --reduction;
        }
        if (!has_tt_move) {
            ++reduction;
        }
        if (prior_fail_high) {
            --reduction;
        }
        if (history_score > 512) {
            --reduction;
        } else if (history_score < -512) {
            ++reduction;
        }
        if (continuation_score > 512) {
            --reduction;
        } else if (continuation_score < -512) {
            ++reduction;
        }
        if (capture_history_score < -512) {
            ++reduction;
        }
        // Stockfish 19 still performs the late-move probe on PV nodes; its
        // PV adjustment makes the probe shallower/less reduced but does not
        // turn it into an unconditional full-depth move. Preserve that
        // distinction in Koi's integer-depth representation.
        const int minimum_reduction = pv_node && full_child_depth > 0 ? 1 : 0;
        reduction = std::clamp(
            reduction, minimum_reduction, std::max(minimum_reduction, full_child_depth));
        return LateMoveDecision{
            candidate, high_history_exclusion, reduction,
            !high_history_exclusion && reducible_quiet && !quiet_forcing && reduction > 0};
    }

    [[nodiscard]] static constexpr ProbCutDecision prob_cut(
        const int depth, const int alpha, const int beta, const int static_eval,
        const bool checked, const bool improving, const bool excluded_move,
        const bool repetition_sensitive) noexcept {
        const bool eligible = !checked && !excluded_move && !repetition_sensitive &&
            depth >= 4 && beta > -kInfinity + 512 && beta < kInfinity - 512 &&
            static_eval >= beta - 160 - depth * 16;
        if (!eligible) {
            return {};
        }
        const int margin = 180 - (improving ? 32 : 0) + std::clamp(beta - alpha, 0, 32);
        return ProbCutDecision{true, beta + margin,
                               std::max(1, depth - (improving ? 4 : 3))};
    }

    [[nodiscard]] static constexpr bool quiet_futility(
        const bool phase_rich_quiet_position, const bool capture,
        const bool gives_check, const bool promotion, const int move_number,
        const int static_eval, const int depth, const int alpha) noexcept {
        return phase_rich_quiet_position && !capture && !gives_check && !promotion &&
            move_number > 0 && static_eval + 80 + depth * 60 <= alpha;
    }

    [[nodiscard]] static constexpr QuiescenceCapturePrune quiescence_capture(
        const bool checked, const bool capture, const bool gives_check,
        const bool promotion, const int see_score, const int captured_piece_value,
        const int best, const int alpha) noexcept {
        if (checked || !capture || gives_check || promotion) {
            return QuiescenceCapturePrune::none;
        }
        if (see_score < 0) {
            return QuiescenceCapturePrune::static_exchange;
        }
        return best + captured_piece_value + 100 < alpha ?
            QuiescenceCapturePrune::delta : QuiescenceCapturePrune::none;
    }
};

} // namespace koi::detail
