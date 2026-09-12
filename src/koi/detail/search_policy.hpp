#pragma once

#include <algorithm>
#include <cmath>

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
        // Match Stockfish 19's static-evaluation guard.  Null move is only a
        // useful proof at a cut node when the side to move is already well
        // above beta; making this gate permissive is a common source of
        // zugzwang and defensive-resource losses.
        const int static_eval_floor = beta + 365 - 13 * depth -
            (improving ? 47 : 0);
        if (!base.eligible || pawn_endgame || beta < -2'000 ||
            static_eval < static_eval_floor) {
            return {};
        }

        const int excess = std::max(0, static_eval - beta);
        // Stockfish 19: R = 7 + depth / 3 + max((eval - beta) / 256, 0).
        // Keep the exact shape; the call site clamps the resulting child
        // depth at qsearch when R exceeds the remaining horizon.
        const int reduction = 7 + depth / 3 + excess / 256;
        return DynamicNullMoveDecision{true, reduction, depth >= 16};
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
         const bool prior_fail_high, const int next_cutoff_count,
         const bool has_tt_move,
         const bool tt_pv) noexcept {
        // Stockfish applies LMR to sufficiently late non-checking captures as
        // well as quiet moves. Captures start one step more conservatively,
        // while their capture history still controls the final reduction.
        const bool candidate = !root_pawn_move && move_number >= 2 && depth >= 4 &&
            !checked && !gives_check && !promotion && !tt_move && !killer &&
            (!capture || capture_history_score > -8'192);
        const bool high_history_exclusion = candidate &&
            high_history_move_excluded_from_lmr(history_score);
        if (!candidate) {
            return LateMoveDecision{false, false, 0, false};
        }

        int reduction = capture ? 0 : 1;
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
        // A TT-PV entry is a stronger ordering signal than an ordinary TT
        // move.  Stockfish's fixed-point LMR formula gives these nodes a
        // compensating reduction decrease after its node-type adjustments;
        // mirror that behavior in the integer-depth policy.
        // The legacy late_move() contract remains unchanged for diagnostics;
        // live search uses this dynamic form with the TT-PV state supplied.
        if (tt_pv) {
            --reduction;
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
        // Stockfish uses the number of fail-high children in the next ply as
        // a direct signal that this move is likely to be a late/less useful
        // continuation. Keep the old one-bit sibling signal as a small
        // compensating hint when that stronger count is unavailable.
        if (next_cutoff_count > 1) {
            ++reduction;
            if (next_cutoff_count > 2) {
                ++reduction;
            }
        } else if (prior_fail_high) {
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
        } else if (capture && capture_history_score > 512) {
            --reduction;
        }
        // Stockfish 19 still performs the late-move probe on PV nodes; its
        // PV adjustment makes the probe shallower/less reduced but does not
        // turn it into an unconditional full-depth move. Preserve that
        // distinction in Koi's integer-depth representation.
        reduction = std::clamp(
            reduction, 0, std::max(0, full_child_depth));
        return LateMoveDecision{
            candidate, high_history_exclusion, reduction,
            !high_history_exclusion && reducible_quiet && !quiet_forcing && reduction > 0};
    }

    [[nodiscard]] static constexpr ProbCutDecision prob_cut(
        const int depth, const int alpha, const int beta, const int static_eval,
        const bool checked, const bool improving, const bool excluded_move,
        const bool repetition_sensitive) noexcept {
        const bool eligible = !checked && !excluded_move && !repetition_sensitive &&
            depth >= 3 && std::abs(beta) < kMateThreshold &&
            beta > -kInfinity + 512 && beta < kInfinity - 512 &&
            static_eval >= beta - 160 - depth * 16;
        if (!eligible) {
            return {};
        }
        const int margin = 241 - (improving ? 64 : 0);
        return ProbCutDecision{true, beta + margin,
                               std::max(0, depth - (improving ? 5 : 3))};
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
        if (see_score < kQuiescenceSeeThreshold) {
            return QuiescenceCapturePrune::static_exchange;
        }
        return best + captured_piece_value + 100 < alpha ?
            QuiescenceCapturePrune::delta : QuiescenceCapturePrune::none;
    }
};

} // namespace koi::detail
