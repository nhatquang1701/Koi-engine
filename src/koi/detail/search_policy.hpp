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

// The pre-make part of the late-move decision.  A candidate's reduction also
// needs facts that only exist after the move is made (quiet forcing features,
// child check state), so search queries the gate before the make and the full
// decision after it; keeping the gate in one place stops the two computations
// from drifting apart.
struct LateMoveGate {
    bool candidate = false;
    bool high_history_exclusion = false;
};

struct ProbCutDecision {
    bool eligible = false;
    int beta = 0;
    int depth = 0;
};

struct ReverseFutilityDecision {
    bool eligible = false;
    int margin = 0;
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
    // can afford a larger null reduction. Every eligible fail-high is then
    // verified before Koi accepts the selective cutoff.
    [[nodiscard]] static constexpr DynamicNullMoveDecision dynamic_null_move(
        const int depth, const int alpha, const int beta, const int static_eval,
        const bool checked, const bool allowed, const bool improving,
        const bool pawn_endgame) noexcept {
        const NullMoveDecision base = null_move(depth, alpha, beta, checked, allowed);
        // Stockfish 19 uses a +365 NNUE-calibrated confidence margin here.
        // Koi's classical evaluator has a narrower positional-score range;
        // retain the same depth/improving slope but calibrate the margin to
        // that scale.  A null fail-high is still re-searched before cutoff,
        // while the node-type, repetition, and sparse-material gates provide
        // the Koi-specific safety envelope.
        const int static_eval_floor = beta + kNullMoveStaticMargin - 13 * depth -
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
        // Verification is reserved for deep nodes, matching Stockfish 19's
        // depth gate.  At those depths the reduced probe is a substantial
        // search of the null-free position, so an unverified fail-high could
        // overstate the bound; below the threshold the null probe itself must
        // already have reached beta without a selective cutoff, and a second
        // full search would only repeat the same shallow horizon.
        const bool verify = reduction >= 3 && depth >= kNullMoveVerificationMinimumDepth;
        return DynamicNullMoveDecision{true, reduction, verify};
    }

    [[nodiscard]] static constexpr bool check_extension(
        const bool checked, const int depth, const bool short_timed_root,
        const int extensions_remaining) noexcept {
        return checked && depth < kMaximumSearchDepth && !short_timed_root &&
            extensions_remaining > 0;
    }

    [[nodiscard]] static constexpr LateMoveGate late_move_gate(
        const int depth, const int move_number, const int history_score,
        const int capture_history_score, const bool root_pawn_move,
        const bool checked, const bool gives_check, const bool capture,
        const bool promotion, const bool tt_move, const bool killer) noexcept {
        // Stockfish applies LMR to sufficiently late non-checking captures as
        // well as quiet moves. Captures start one step more conservatively,
        // while their capture history still controls the final reduction.
        const bool candidate = !root_pawn_move && move_number >= 2 && depth >= 3 &&
            !checked && !gives_check && !promotion && !tt_move && !killer &&
            (!capture || capture_history_score > -8'192);
        const bool high_history_exclusion = candidate &&
            high_history_move_excluded_from_lmr(history_score);
        return LateMoveGate{candidate, high_history_exclusion};
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
        const LateMoveGate gate = late_move_gate(
            depth, move_number, history_score, capture_history_score,
            root_pawn_move, checked, gives_check, capture, promotion, tt_move,
            killer);
        const bool candidate = gate.candidate;
        const bool high_history_exclusion = gate.high_history_exclusion;
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

    // Reverse futility is a static fail-high shortcut.  It is useful when a
    // quiet scout node is already comfortably above beta, but unlike a TT
    // bound it is an evaluation-based estimate and must remain selective.
    // The live call site supplies the position-sensitive safety envelope;
    // this function keeps the depth and improving-state calibration pure.
    [[nodiscard]] static constexpr ReverseFutilityDecision reverse_futility(
        const int depth, const int beta, const int static_eval,
        const bool checked, const bool pv_node, const bool allowed,
        const bool improving, const bool opponent_worsening) noexcept {
        const bool eligible = allowed && !checked && !pv_node &&
            depth >= kReverseFutilityMinimumDepth &&
            depth <= kReverseFutilityMaximumDepth &&
            std::abs(beta) < kMateThreshold &&
            std::abs(static_eval) < kMateThreshold && static_eval >= beta;
        if (!eligible) {
            return {};
        }
        const int margin = std::max(
            0, kReverseFutilityBaseMargin + kReverseFutilityDepthMargin * depth -
                (improving ? kReverseFutilityImprovingDiscount : 0) +
                (opponent_worsening ? kReverseFutilityWorseningSurcharge : 0));
        return ReverseFutilityDecision{static_eval - margin >= beta, margin};
    }

    [[nodiscard]] static constexpr bool quiet_futility(
        const bool phase_rich_quiet_position, const bool capture,
        const bool gives_check, const bool promotion, const int move_number,
        const int static_eval, const int depth, const int alpha) noexcept {
        return phase_rich_quiet_position && !capture && !gives_check && !promotion &&
            move_number > 0 && static_eval + 80 + depth * 60 <= alpha;
    }

    // A very negative continuation-history score is evidence that this move
    // repeatedly failed in the same predecessor context.  Use that signal
    // only for late quiet moves at interior scout nodes; the call site adds
    // the position-sensitive repetition, material, and special-move guards.
    [[nodiscard]] static constexpr bool negative_continuation_history(
        const int depth, const int move_number, const int continuation_score,
        const bool pv_node, const bool checked, const bool capture,
        const bool gives_check, const bool promotion, const bool tt_move,
        const bool killer, const bool counter_move, const bool allowed) noexcept {
        return allowed && !pv_node && !checked && !capture && !gives_check &&
            !promotion && !tt_move && !killer && !counter_move &&
            depth >= kNegativeContinuationHistoryMinimumDepth &&
            move_number >= kNegativeContinuationHistoryMinimumMove &&
            continuation_score < -kNegativeContinuationHistoryBase * depth;
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
