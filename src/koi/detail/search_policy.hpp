#pragma once

#include <algorithm>

#include "koi/detail/search_constants.hpp"

namespace koi::detail {

struct NullMoveDecision {
    bool eligible = false;
    int reduction = 0;
};

struct LateMoveDecision {
    bool candidate = false;
    bool high_history_exclusion = false;
    int reduction = 0;
    bool reduced = false;
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
