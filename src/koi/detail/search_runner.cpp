#include "koi/detail/search_runner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "koi/detail/root_coordinator.hpp"
#include "koi/detail/search_context.hpp"
#include "koi/detail/search_ordering.hpp"
#include "koi/detail/search_session.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/search_table_access.hpp"
#include "koi/evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/syzygy_tablebase.hpp"
#include "koi/gpu/nnue_gpu_evaluator.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi::detail {

namespace {

constexpr int kAspirationWindow = 50;
constexpr int kRootSelectiveDepth = 3;
constexpr int kRootSelectiveMargin = 20;
constexpr int kRootSelectiveImprovement = 15;
constexpr std::size_t kMaximumRootCheckConfirmationCandidates = 2;
// A depth-two root commonly has enough work for PVS scouts to establish a
// useful ordering, but not enough horizon to make every scout fail-low an
// exact score.  Recheck a small, score-banded set at the same depth so root
// ranking is based on comparable full-window results rather than a mixture of
// exact scores and upper bounds.
constexpr int kRootShallowConfirmationDepth = 2;
constexpr int kRootShallowConfirmationMargin = 32;
constexpr int kRootShallowForcingMargin = 200;
constexpr std::size_t kMaximumRootShallowConfirmationCandidates = 4;
constexpr int kRootKingSafetySelectiveDepth = 5;
constexpr int kRootKingSafetyTieMargin = 50;
constexpr std::size_t kMaximumRootKingSafetyCandidates = 2;
// If a checked-root search is interrupted before any iteration completes,
// static material can make a king capture look much better than a safer
// escape.  Treat king-zone exposure as a crisis tie-break, but only inside
// this emergency path and only when the raw scores are reasonably close.
constexpr int kEmergencyKingEscapeSafetyMargin = 650;
constexpr int kEmergencyKingZoneAttackWeight = 400;
constexpr int kEmergencyKingCaptureRisk = 160;
constexpr int kEmergencyQuietForcingMargin = 200;
// A narrow check can look artificially dominant in the bounded fallback
// because its small evasion set lets the one-ply oracle see the same tactical
// resource repeatedly.  Allow a sound, non-hanging major-piece check with a
// larger evasion set to displace that narrow check when the raw scores are
// still close.  This remains local to an interrupted root; completed search
// keeps the normal score ordering.
constexpr int kEmergencyBroadCheckReplacementMargin = 400;
constexpr int kEmergencySafeCaptureTieMargin = 32;
constexpr int kEmergencySafeExchangeTieMargin = 80;
// In an interrupted root, an irreversible quiet pawn push that leaves an
// immediate checking resource is more dangerous than a similarly scored
// piece move. Keep this discount local to the unsafe fallback ranking; normal
// completed search and ordinary pawn evaluation are unchanged.
constexpr int kEmergencyUnsafePawnQuietPenalty = 75;
constexpr std::size_t kMaximumEmergencyForcingCheckEvasions = 3;
constexpr std::size_t kMaximumEmergencyQuietForcingResearches = 16;
constexpr std::size_t kMaximumEmergencyRootEvasionReplies = 6;
// The emergency static fallback is deliberately limited to the middle of the
// short-search band. At very low requested movetime the fixed overhead/safety
// ceiling leaves too little budget for both fallback scoring and a root pass;
// at the serial-path cutoff, scoring every root move can likewise starve the
// first authoritative iteration.
constexpr std::size_t kMaximumMultiPv = 16;

} // namespace

std::size_t normalized_threads(std::size_t threads) noexcept {
    return std::clamp(threads, std::size_t{1}, maximum_search_threads());
}

std::uint8_t normalized_speed(std::uint8_t speed_percent) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::uint32_t>(speed_percent, 1, 100));
}

std::size_t normalized_multi_pv(std::size_t multi_pv) noexcept {
    return std::clamp(multi_pv, std::size_t{1}, kMaximumMultiPv);
}

std::uint32_t normalized_move_overhead(std::uint32_t move_overhead_ms) noexcept {
    return std::min<std::uint32_t>(move_overhead_ms, 5'000);
}

std::uint32_t normalized_slow_mover(std::uint32_t slow_mover_percent) noexcept {
    return std::clamp<std::uint32_t>(slow_mover_percent, 10, 1'000);
}

std::uint32_t normalized_elo(std::uint32_t elo) noexcept {
    return std::clamp<std::uint32_t>(elo, 1'320, 3'190);
}

namespace {

std::optional<int> mate_from_score(int score) noexcept {
    if (score >= kMateThreshold) {
        return (kMateScore - score + 1) / 2;
    }
    if (score <= -kMateThreshold) {
        return -((kMateScore + score + 1) / 2);
    }
    return std::nullopt;
}

void accumulate_stats(SearchStats& total, const SearchStats& partial) noexcept {
    total.nodes += partial.nodes;
    total.qnodes += partial.qnodes;
    total.qsearch_cache_hits += partial.qsearch_cache_hits;
    total.position_feature_extractions += partial.position_feature_extractions;
    total.lmr_parent_feature_reuses += partial.lmr_parent_feature_reuses;
    total.evaluation_cache_hits += partial.evaluation_cache_hits;
    total.correction_history_updates += partial.correction_history_updates;
    total.tt_hits += partial.tt_hits;
    total.pvs_searches += partial.pvs_searches;
    total.pvs_researches += partial.pvs_researches;
    total.root_pvs_searches += partial.root_pvs_searches;
    total.root_pvs_researches += partial.root_pvs_researches;
    total.root_selective_candidates += partial.root_selective_candidates;
    total.root_selective_researches += partial.root_selective_researches;
    total.short_fallback_invocations += partial.short_fallback_invocations;
    total.short_fallback_candidates += partial.short_fallback_candidates;
    total.short_fallback_overdue_candidates += partial.short_fallback_overdue_candidates;
    total.quiet_forcing_extensions += partial.quiet_forcing_extensions;
    total.aspiration_researches += partial.aspiration_researches;
    total.check_extensions += partial.check_extensions;
    total.king_safety_extensions += partial.king_safety_extensions;
    total.qchecks += partial.qchecks;
    total.see_prunes += partial.see_prunes;
    total.delta_prunes += partial.delta_prunes;
    total.null_cutoffs += partial.null_cutoffs;
    total.null_verifications += partial.null_verifications;
    total.null_repetition_skips += partial.null_repetition_skips;
    total.lmr_reductions += partial.lmr_reductions;
    total.lmr_verifications += partial.lmr_verifications;
    total.lmr_king_zone_exclusions += partial.lmr_king_zone_exclusions;
    total.lmr_high_history_exclusions += partial.lmr_high_history_exclusions;
    total.continuation_history_prunes += partial.continuation_history_prunes;
    total.quiet_futility_prunes += partial.quiet_futility_prunes;
    total.reverse_futility_prunes += partial.reverse_futility_prunes;
    total.razoring_prunes += partial.razoring_prunes;
    total.internal_iterative_deepening += partial.internal_iterative_deepening;
    total.probcut_searches += partial.probcut_searches;
    total.probcut_cutoffs += partial.probcut_cutoffs;
    total.singular_searches += partial.singular_searches;
    total.singular_extensions += partial.singular_extensions;
    total.multi_cut_prunes += partial.multi_cut_prunes;
    total.capture_history_updates += partial.capture_history_updates;
    total.quiet_history_updates += partial.quiet_history_updates;
    total.continuation_history_updates += partial.continuation_history_updates;
    total.tbhits += partial.tbhits;
    total.seldepth = std::max(total.seldepth, partial.seldepth);
}

int apply_claimable_draw_option(const GameState& state, const Color root_color,
                                const int score) noexcept {
    // Preserve the interruption sentinel: it is control flow, not a chess
    // score that can be replaced by the draw option.
    if (score <= -kInfinity || !state.is_claimable_draw()) {
        return score;
    }
    return state.side_to_move() == root_color ?
        std::max(0, score) : std::min(0, score);
}

int short_fallback_evaluate(const Evaluator& evaluator, const GameState& state,
                            const Color root_color, std::mutex* evaluator_mutex) {
    int score = 0;
    if (evaluator_mutex != nullptr) {
        std::lock_guard lock(*evaluator_mutex);
        score = evaluator.evaluate(state, root_color);
    } else {
        score = evaluator.evaluate(state, root_color);
    }
    return apply_claimable_draw_option(state, root_color, score);
}

struct FallbackControl {
    const TimeManager* time_manager = nullptr;
    const std::atomic_bool* stop_requested = nullptr;

    [[nodiscard]] bool interrupted() const noexcept {
        if (stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed)) {
            return true;
        }
        return time_manager != nullptr && time_manager->should_stop(0);
    }
};

// Emergency scanners evaluate a position from the original root's
// perspective, while the side to move changes after every copied move.  Keep
// terminal ordering in one place: a no-legal-move mate must win over a rule
// draw, and a stalemate must remain neutral.  Returning nullopt means the
// position is live and not currently draw-terminated.
std::optional<int> fallback_terminal_or_draw_score(
    const GameState& state, const Color root_color, const int mate_distance) {
    const std::vector<Move> legal_moves = state.legal_moves();
    if (legal_moves.empty()) {
        if (!state.in_check()) {
            return 0;
        }
        return state.side_to_move() == root_color ?
            -kMateScore + mate_distance : kMateScore - mate_distance;
    }
    // Claimable draws are not terminal: the emergency scanner must continue
    // through legal moves and let the side to move choose the zero option.
    if (state.is_forced_draw()) {
        return 0;
    }
    return std::nullopt;
}

std::optional<int> emergency_king_escape_risk(
    const GameState& root, const MoveMetadata& metadata,
    const GameState& after_move) noexcept {
    if (!root.in_check() || metadata.moving_piece != PieceType::king) {
        return std::nullopt;
    }

    const std::size_t side = root.side_to_move() == Color::white ? 0U : 1U;
    const auto features = after_move.position_features();
    return static_cast<int>(features.king_zone_attacks[side]) *
            kEmergencyKingZoneAttackWeight +
        (metadata.is_capture() ? kEmergencyKingCaptureRisk : 0);
}

int quiet_forcing_fallback_score(
    const GameState& after_move, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_move, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList opponent_moves;
    after_move.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    if (opponent_moves.empty()) {
        // after_move is the position immediately after the root candidate,
        // so no legal reply while in check means the root side delivered
        // mate, not that it was mated.
        return after_move.in_check() ? kMateScore - 1 : 0;
    }

    int worst_score = kInfinity;
    bool found_reply = false;
    for (const MoveMetadata& opponent_move : opponent_moves) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_opponent = after_move;
        if (!after_opponent.make_search_move(opponent_move)) {
            continue;
        }

        if (const auto terminal = fallback_terminal_or_draw_score(
                after_opponent, root_color, 2); terminal.has_value()) {
            worst_score = std::min(worst_score, *terminal);
            found_reply = true;
            continue;
        }

        MoveMetadataList responses;
        after_opponent.legal_moves_with_metadata(
            responses, true, false, CheckFlagMode::all_moves);
        if (responses.empty()) {
            worst_score = std::min(
                worst_score, after_opponent.in_check() ? -kMateScore + 2 : 0);
            found_reply = true;
            continue;
        }

        // The root side may already be able to claim a draw after the
        // opponent's reply.  That claim is a real zero-valued response and
        // must compete with the legal continuations below; otherwise a
        // short fallback can incorrectly treat an all-losing response set as
        // an interruption sentinel and let the opponent's minimization hide
        // the draw option.
        int best_response_score = after_opponent.is_claimable_draw() ? 0 : -kInfinity;
        for (const MoveMetadata& response : responses) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_response = after_opponent;
            if (!after_response.make_search_move(response)) {
                continue;
            }
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_response, root_color, 2); terminal.has_value()) {
                best_response_score = std::max(best_response_score, *terminal);
                continue;
            }
            best_response_score = std::max(
                best_response_score,
                short_fallback_evaluate(evaluator, after_response,
                                        root_color, evaluator_mutex));
        }
        if (best_response_score > -kInfinity) {
            worst_score = std::min(worst_score, best_response_score);
            found_reply = true;
        }
    }
    return apply_claimable_draw_option(
        after_move, root_color, found_reply ? worst_score : -kInfinity);
}

int checked_root_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? kMateScore - 1 : 0;
    }

    int worst_evasion_score = kInfinity;
    bool found_evasion = false;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            worst_evasion_score = std::min(worst_evasion_score, *terminal);
            found_evasion = true;
            continue;
        }
        // A static score immediately after the evasion rewards a checking
        // move even when the checked side's reply leaves a quiet, strong
        // continuation for the original side.  Give the original side one
        // bounded reply after every legal evasion before comparing checking
        // root candidates.  This remains confined to the emergency short
        // fallback; the normal search and qsearch retain their usual depth.
        MoveMetadataList replies;
        after_evasion.legal_moves_with_metadata(
            replies, true, false, CheckFlagMode::all_moves);
        if (replies.empty()) {
            worst_evasion_score = std::min(
                worst_evasion_score,
                after_evasion.in_check() ? -kMateScore + 2 : 0);
            found_evasion = true;
            continue;
        }

        int best_reply_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        for (const MoveMetadata& reply : replies) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_reply = after_evasion;
            if (!after_reply.make_search_move(reply)) {
                continue;
            }
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_reply, root_color, 2); terminal.has_value()) {
                best_reply_score = std::max(best_reply_score, *terminal);
                continue;
            }
            int reply_score = short_fallback_evaluate(
                evaluator, after_reply, root_color, evaluator_mutex);
            if (after_reply.in_check() && after_reply.legal_moves().empty()) {
                reply_score = kMateScore - 2;
            }
            best_reply_score = std::max(best_reply_score, reply_score);
        }
        worst_evasion_score = std::min(worst_evasion_score, best_reply_score);
        found_evasion = true;
    }
    return apply_claimable_draw_option(
        after_check, root_color, found_evasion ? worst_evasion_score : -kInfinity);
}

bool hanging_major_piece_check(const GameState& after_check,
                               const MoveMetadata& checking_move) noexcept {
    if (!after_check.in_check() ||
        (checking_move.moving_piece != PieceType::queen &&
         checking_move.moving_piece != PieceType::rook)) {
        return false;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, false, CheckFlagMode::all_moves);
    return std::any_of(evasions.begin(), evasions.end(),
                       [&checking_move](const MoveMetadata& evasion) {
                           return evasion.is_capture() &&
                               evasion.move.to() == checking_move.move.to();
                       });
}

int shallow_checked_root_fallback_score(
    const GameState& after_check, const MoveMetadata& checking_move,
    const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 1); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? kMateScore - 1 : 0;
    }

    // The checking move has just been made by the root side, so every
    // evasion belongs to the opponent.  Select the worst root-perspective
    // result, not the best one; using max here lets one attractive evasion
    // hide a stronger defensive answer and makes a shallow check look
    // artificially decisive.
    int worst_evasion_score = kInfinity;
    bool found_evasion = false;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            worst_evasion_score = std::min(worst_evasion_score, *terminal);
            found_evasion = true;
            continue;
        }
        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        const bool captures_checking_piece = evasion.is_capture() &&
            evasion.move.to() == checking_move.move.to();
        const bool valuable_hanging_checker = captures_checking_piece &&
            (checking_move.moving_piece == PieceType::queen ||
             checking_move.moving_piece == PieceType::rook);
        if (valuable_hanging_checker) {
            // A checking queen/rook that can be taken safely is not a useful
            // emergency candidate. Keep a genuine mate-in-one sacrifice
            // available, but make a plainly hanging checker lose its tie
            // against quiet defensive moves.
            evasion_score = std::min(evasion_score, -3'000);
        }
        if (after_evasion.in_check() && after_evasion.legal_moves().empty()) {
            // The root side is to move after the opponent's evasion.  If
            // that evasion also checks the root and leaves no legal move,
            // the original checking candidate has been refuted by mate.
            evasion_score = -kMateScore + 2;
        }
        worst_evasion_score = std::min(worst_evasion_score, evasion_score);
        found_evasion = true;
    }
    return apply_claimable_draw_option(
        after_check, root_color, found_evasion ? worst_evasion_score : -kInfinity);
}

int forcing_reply_fallback_score(const GameState& after_forcing_reply,
                                 Color root_color, const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 PieceType captured_piece,
                                 const FallbackControl* control);

int king_evasion_fallback_score(
    const GameState& after_evasion, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const bool is_capture,
    const FallbackControl* control = nullptr) {
    // This helper is evaluated from Koi's perspective after the opponent has
    // checked. Do not reuse shallow_checked_root_fallback_score here: that
    // helper intentionally penalizes a capturable *root* checker, whereas a
    // capturable opponent checker is favorable to Koi.
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    const PositionFeatures evasion_features = after_evasion.position_features();
    const std::size_t root_side = root_color == Color::white ? 0U : 1U;
    const int king_exposure_penalty =
        static_cast<int>(evasion_features.king_zone_attacks[root_side]) *
            kEmergencyKingZoneAttackWeight +
        (is_capture ? kEmergencyKingCaptureRisk : 0);
    const int static_score = short_fallback_evaluate(
        evaluator, after_evasion, root_color, evaluator_mutex) - king_exposure_penalty;
    int worst_score = kInfinity;
    bool found_check = false;
    MoveMetadataList opponent_moves;
    after_evasion.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    for (const MoveMetadata& opponent_move : opponent_moves) {
        const bool forcing_reply = opponent_move.gives_check ||
            opponent_move.is_capture() ||
            opponent_move.move.promotion() != Promotion::none;
        if (!forcing_reply) {
            continue;
        }
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_check = after_evasion;
        if (!after_check.make_search_move(opponent_move)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_check, root_color, 1); terminal.has_value()) {
            found_check = true;
            worst_score = std::min(worst_score, *terminal);
            continue;
        }
        if (!opponent_move.gives_check) {
            const int forcing_score = forcing_reply_fallback_score(
                after_check, root_color, evaluator, evaluator_mutex,
                opponent_move.captured_piece, control);
            if (forcing_score <= -kInfinity) {
                return -kInfinity;
            }
            found_check = true;
            worst_score = std::min(worst_score, forcing_score);
            continue;
        }
        found_check = true;
        MoveMetadataList evasions;
        after_check.legal_moves_with_metadata(
            evasions, true, true, CheckFlagMode::all_moves);
        if (evasions.empty()) {
            const int check_score = after_check.in_check() ? -kMateScore + 1 : 0;
            worst_score = std::min(worst_score, check_score);
            continue;
        }
        int best_evasion_score = -kInfinity;
        for (const MoveMetadata& evasion : evasions) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_check_evasion = after_check;
            if (!after_check_evasion.make_search_move(evasion)) {
                continue;
            }
            int evasion_score = 0;
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_check_evasion, root_color, 2); terminal.has_value()) {
                evasion_score = *terminal;
            } else {
                evasion_score = short_fallback_evaluate(
                    evaluator, after_check_evasion, root_color, evaluator_mutex);
            }
            best_evasion_score = std::max(best_evasion_score, evasion_score);
        }
        // A king move that leaves the opponent a check with only one or two
        // legal answers is usually a confinement/continuation trap.  The
        // emergency scanner cannot afford a full mate search here, so charge
        // a bounded pressure penalty for a very narrow evasion set.  Normal
        // completed search is unaffected and still evaluates the position at
        // its ordinary depth.
        const int confinement_penalty = evasions.size() == 1 ? 900 :
            (evasions.size() == 2 ? 250 : 0);
        const int check_score = best_evasion_score - confinement_penalty;
        if (check_score <= -kInfinity) {
            return -kInfinity;
        }
        worst_score = std::min(worst_score, check_score);
    }
    return apply_claimable_draw_option(
        after_evasion, root_color,
        found_check ? worst_score - king_exposure_penalty : static_score);
}

int checked_reply_fallback_score(const GameState& after_check, const Color root_color,
                                 const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 2); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, false, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? -kMateScore + 2 : 0;
    }

    int best_evasion_score = -kInfinity;
    for (const MoveMetadata& evasion : evasions) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }

        const auto evasion_terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2);
        if (evasion_terminal.has_value()) {
            best_evasion_score = std::max(best_evasion_score, *evasion_terminal);
            continue;
        }
        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        if (after_evasion.in_check()) {
            MoveMetadataList replies;
            after_evasion.legal_moves_with_metadata(
                replies, true, false, CheckFlagMode::all_moves);
            if (replies.empty()) {
                evasion_score = kMateScore - 2;
            }
        } else {
            MoveMetadataList opponent_moves;
            after_evasion.legal_moves_with_metadata(
                opponent_moves, true, false, CheckFlagMode::all_moves);
            for (const MoveMetadata& opponent_move : opponent_moves) {
                if (control != nullptr && control->interrupted()) {
                    return -kInfinity;
                }
                const bool forcing_reply = opponent_move.gives_check ||
                    opponent_move.is_capture() ||
                    opponent_move.move.promotion() != Promotion::none;
                if (!forcing_reply) {
                    continue;
                }
                GameState after_forcing_reply = after_evasion;
                if (!after_forcing_reply.make_search_move(opponent_move)) {
                    continue;
                }
                const auto forcing_terminal = fallback_terminal_or_draw_score(
                    after_forcing_reply, root_color, 2);
                const int forcing_score = forcing_terminal.has_value() ? *forcing_terminal :
                    forcing_reply_fallback_score(
                        after_forcing_reply, root_color, evaluator, evaluator_mutex,
                        opponent_move.captured_piece, control);
                evasion_score = std::min(evasion_score, forcing_score);
            }
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return apply_claimable_draw_option(after_check, root_color, best_evasion_score);
}

int root_evasion_fallback_score(
    const GameState& after_evasion, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    // This helper is entered after the root side has answered an initial
    // check, so the opponent is to move.  `checked_reply_fallback_score()`
    // has the opposite contract: it expects the side to move to be in check.
    // Keep the minimax direction explicit here so a non-king evasion is not
    // accidentally evaluated as though it were itself a checked position.
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_evasion, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    MoveMetadataList opponent_moves;
    after_evasion.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    if (opponent_moves.empty()) {
        return after_evasion.in_check() ? kMateScore - 2 : 0;
    }

    int score = short_fallback_evaluate(
        evaluator, after_evasion, root_color, evaluator_mutex);
    std::size_t examined_forcing_replies = 0;
    for (const MoveMetadata& opponent_move : opponent_moves) {
        const bool forcing = opponent_move.gives_check ||
            opponent_move.is_capture() ||
            opponent_move.move.promotion() != Promotion::none;
        if (!forcing) {
            continue;
        }
        if (control == nullptr && examined_forcing_replies++ >=
                kMaximumEmergencyRootEvasionReplies) {
            // The no-control form is used after a very short deadline, where
            // completion matters more than exhaustive emergency scanning.
            // Keep that path bounded even if the opponent has a wide forcing
            // move set.
            break;
        }
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        GameState after_forcing = after_evasion;
        if (!after_forcing.make_search_move(opponent_move)) {
            continue;
        }
        const auto forcing_terminal = fallback_terminal_or_draw_score(
            after_forcing, root_color, 2);
        const int forcing_score = forcing_terminal.has_value() ? *forcing_terminal :
            (opponent_move.gives_check ?
                checked_reply_fallback_score(
                    after_forcing, root_color, evaluator, evaluator_mutex, control) :
                forcing_reply_fallback_score(
                    after_forcing, root_color, evaluator, evaluator_mutex,
                    opponent_move.captured_piece, control));
        if (forcing_score <= -kInfinity) {
            return -kInfinity;
        }
        score = std::min(score, forcing_score);
    }
    return apply_claimable_draw_option(after_evasion, root_color, score);
}

int overdue_check_reply_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex) {
    constexpr std::size_t kMaximumEvasions = 2;
    constexpr std::size_t kMaximumForcingReplies = 1;

    if (const auto terminal = fallback_terminal_or_draw_score(
            after_check, root_color, 2); terminal.has_value()) {
        return *terminal;
    }

    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? -kMateScore + 2 : 0;
    }

    // Examine king moves and captures before quiet interpositions so the
    // bounded continuation is more likely to see the critical queen escape
    // or recapture without scanning every evasion at full depth.
    const auto evasion_priority = [](const MoveMetadata& move) {
        if (move.is_capture()) {
            return 0;
        }
        if (move.moving_piece == PieceType::king) {
            return 1;
        }
        if (move.moving_piece == PieceType::queen ||
            move.moving_piece == PieceType::rook) {
            return 2;
        }
        return 3;
    };
    std::stable_sort(evasions.begin(), evasions.end(),
                     [&](const MoveMetadata& left, const MoveMetadata& right) {
                         return evasion_priority(left) < evasion_priority(right);
                     });

    int best_evasion_score = -kInfinity;
    std::size_t evasion_count = 0;
    for (const MoveMetadata& evasion : evasions) {
        if (evasion_count++ >= kMaximumEvasions) {
            break;
        }
        GameState after_evasion = after_check;
        if (!after_evasion.make_search_move(evasion)) {
            continue;
        }
        if (const auto terminal = fallback_terminal_or_draw_score(
                after_evasion, root_color, 2); terminal.has_value()) {
            best_evasion_score = std::max(best_evasion_score, *terminal);
            continue;
        }

        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        if (after_evasion.in_check()) {
            if (after_evasion.legal_moves().empty()) {
                // The evasion belongs to the root side.  A checking evasion
                // that mates the opponent is a win for the root, not a loss.
                evasion_score = kMateScore - 2;
            }
            best_evasion_score = std::max(best_evasion_score, evasion_score);
            continue;
        }

        MoveMetadataList replies;
        after_evasion.legal_moves_with_metadata(
            replies, true, true, CheckFlagMode::all_moves);
        std::array<const MoveMetadata*, kMaximumForcingReplies> selected_replies{
            nullptr};
        std::size_t selected_count = 0;
        const auto append_reply = [&](const auto& predicate) {
            if (selected_count >= selected_replies.size()) {
                return;
            }
            const auto selected = std::find_if(
                replies.begin(), replies.end(),
                [&](const MoveMetadata& move) {
                    if (std::find(selected_replies.begin(),
                                  selected_replies.begin() + selected_count,
                                  &move) != selected_replies.begin() + selected_count) {
                        return false;
                    }
                    return predicate(move);
                });
            if (selected != replies.end()) {
                selected_replies[selected_count++] = &*selected;
            }
        };
        append_reply([](const MoveMetadata& move) {
            return move.is_capture() &&
                (move.captured_piece == PieceType::queen ||
                 move.captured_piece == PieceType::rook);
        });

        for (std::size_t reply_index = 0; reply_index < selected_count; ++reply_index) {
            const MoveMetadata& reply = *selected_replies[reply_index];
            GameState after_reply = after_evasion;
            if (!after_reply.make_search_move(reply)) {
                continue;
            }
            const auto reply_terminal = fallback_terminal_or_draw_score(
                after_reply, root_color, 2);
            int reply_score = reply_terminal.has_value() ? *reply_terminal :
                forcing_reply_fallback_score(
                    after_reply, root_color, evaluator, evaluator_mutex,
                    reply.captured_piece, nullptr);
            if (!reply_terminal.has_value() && reply.is_capture() &&
                       (reply.captured_piece == PieceType::queen ||
                        reply.captured_piece == PieceType::rook)) {
                // The overdue continuation is an emergency safety oracle,
                // not a complete search.  Do not let a shallow evaluation
                // after a major-piece capture erase the material loss that
                // the continuation just established.
                reply_score = std::min(
                    reply_score, -piece_value(reply.captured_piece));
            }
            // After the root side has answered the check, it is the
            // opponent's turn.  A forcing reply therefore minimizes the
            // root-perspective score; taking max(static, reply) would let a
            // shallow static evaluation hide a decisive rook/queen capture.
            evasion_score = std::min(evasion_score, reply_score);
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return apply_claimable_draw_option(after_check, root_color, best_evasion_score);
}

int forcing_reply_fallback_score(const GameState& after_forcing_reply,
                                 const Color root_color, const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const PieceType captured_piece,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
    }
    if (const auto terminal = fallback_terminal_or_draw_score(
            after_forcing_reply, root_color, 2); terminal.has_value()) {
        return *terminal;
    }
    MoveMetadataList responses;
    after_forcing_reply.legal_moves_with_metadata(
        responses, true, false, CheckFlagMode::all_moves);
    if (responses.empty()) {
        return after_forcing_reply.in_check() ? -kMateScore + 2 : 0;
    }

    int best_score = short_fallback_evaluate(
        evaluator, after_forcing_reply, root_color, evaluator_mutex);
    // A shallow static evaluation can miss the significance of a forcing
    // move that has just taken a major root piece, especially when the root
    // still has checking or material counterplay. Keep this emergency
    // comparison conservative unless a forcing reply or mate compensates for
    // the loss.
    const bool has_forcing_loss_floor = captured_piece == PieceType::queen ||
        captured_piece == PieceType::rook || captured_piece == PieceType::bishop ||
        captured_piece == PieceType::knight;
    const int forcing_loss_floor = captured_piece == PieceType::queen ? -1'200 :
        captured_piece == PieceType::rook ? -650 : -425;
    if (has_forcing_loss_floor) {
        // A pawn capture (and a non-capturing promotion, represented by
        // `none`) still has a real static continuation when the defender's
        // replies are quiet.  Do not turn that ordinary baseline into the
        // interruption sentinel merely because no major/minor material floor
        // applies.
        best_score = std::min(best_score, forcing_loss_floor);
    }
    for (const MoveMetadata& response : responses) {
        if (control != nullptr && control->interrupted()) {
            return -kInfinity;
        }
        if (!response.is_capture() && !response.gives_check &&
            response.move.promotion() == Promotion::none) {
            continue;
        }
        GameState after_response = after_forcing_reply;
        if (!after_response.make_search_move(response)) {
            continue;
        }
        const auto response_terminal = fallback_terminal_or_draw_score(
            after_response, root_color, 2);
        int response_score = response_terminal.has_value() ? *response_terminal :
            short_fallback_evaluate(evaluator, after_response, root_color, evaluator_mutex);
        if (!response_terminal.has_value() && response.is_capture() &&
            captured_piece == PieceType::none &&
            (response.captured_piece == PieceType::rook ||
             response.captured_piece == PieceType::queen)) {
            // A checking sacrifice is often answered by capturing the checker,
            // but that capture can itself be refuted by an immediate recapture.
            // The interrupted-root fallback must not treat the material-winning
            // middle position as stable before seeing that one tactical reply.
            MoveMetadataList counter_responses;
            after_response.legal_moves_with_metadata(
                counter_responses, true, false, CheckFlagMode::all_moves);
            for (const MoveMetadata& counter_response : counter_responses) {
                if (control != nullptr && control->interrupted()) {
                    return -kInfinity;
                }
                if (!counter_response.is_capture() ||
                    counter_response.move.to() != response.move.to()) {
                    continue;
                }
                GameState after_counter = after_response;
                if (!after_counter.make_search_move(counter_response)) {
                    continue;
                }
                const auto counter_terminal = fallback_terminal_or_draw_score(
                    after_counter, root_color, 3);
                const int counter_score = counter_terminal.has_value() ? *counter_terminal :
                    short_fallback_evaluate(
                        evaluator, after_counter, root_color, evaluator_mutex);
                response_score = std::min(
                    response_score,
                    counter_score);
            }
        }
        best_score = std::max(best_score, response_score);
    }
    return apply_claimable_draw_option(after_forcing_reply, root_color, best_score);
}

bool root_move_is_broad_quiet_check(
    const GameState& root, const MoveMetadata& metadata) noexcept {
    if (!metadata.gives_check || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }
    try {
        GameState after_check = root;
        return after_check.make_search_move(metadata) && after_check.in_check() &&
            after_check.legal_moves().size() > kMaximumEmergencyForcingCheckEvasions;
    } catch (...) {
        return false;
    }
}

std::optional<Move> first_safe_short_search_move(
    const GameState& root, const MoveMetadataList& legal_moves) noexcept {
    try {
        for (const MoveMetadata& metadata : legal_moves) {
            if (root_move_is_broad_quiet_check(root, metadata)) {
                continue;
            }
            if (!root_move_exposes_immediate_check(root, metadata)) {
                return metadata.move;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

bool is_safe_equal_non_pawn_exchange(const MoveMetadata& metadata) noexcept;
bool short_quiet_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept;
bool quiet_move_leaves_safe_capture(
    const MoveMetadata& candidate, const MoveMetadataList& opponent_moves) noexcept;

struct ShortFallbackChoice {
    Move move = Move::no_move();
    int score = -kInfinity;
};

std::optional<ShortFallbackChoice> scored_safe_checking_capture(
    const GameState& root, const MoveMetadataList& legal_moves,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const bool allow_deep_fallback, const FallbackControl* control = nullptr) noexcept {
    try {
        for (const MoveMetadata& metadata : legal_moves) {
            if (control != nullptr && control->interrupted()) {
                return std::nullopt;
            }
            if (!metadata.is_capture() || !metadata.see_computed || metadata.see_score < 0 ||
                (!metadata.gives_check && !root.move_gives_check(metadata.move))) {
                continue;
            }

            GameState after_check = root;
            if (!after_check.make_search_move(metadata)) {
                continue;
            }
            // The move itself may end the game.  Preserve an immediate mate,
            // but do not nominate a stalemate or rule-draw move as a tactical
            // replacement for a completed root line.
            if (const auto terminal = fallback_terminal_or_draw_score(
                    after_check, root.side_to_move(), 1); terminal.has_value()) {
                if (*terminal >= kMateThreshold) {
                    return ShortFallbackChoice{metadata.move, *terminal};
                }
                continue;
            }

            const std::vector<Move> evasions = after_check.legal_moves();
            if (evasions.empty()) {
                return ShortFallbackChoice{
                    metadata.move, after_check.in_check() ? kMateScore - 1 : 0};
            }

            const bool deadline_seen = control != nullptr && control->interrupted();
            const bool use_deep_fallback = allow_deep_fallback && !deadline_seen;
            const FallbackControl* score_control = use_deep_fallback ? control : nullptr;
            const int score = use_deep_fallback ?
                checked_root_fallback_score(
                    after_check, root.side_to_move(), evaluator, evaluator_mutex, score_control) :
                shallow_checked_root_fallback_score(
                    after_check, metadata, root.side_to_move(), evaluator, evaluator_mutex,
                    score_control);
            if (score <= -kInfinity && control != nullptr && control->interrupted()) {
                return std::nullopt;
            }
            if (score > -kInfinity) {
                // A move-only safety helper is allowed to nominate this
                // candidate, but it must publish the score obtained from the
                // candidate's own checked continuation.
                return ShortFallbackChoice{metadata.move, score};
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<ShortFallbackChoice> short_search_fallback_move(
    const GameState& root, const MoveMetadataList& legal_moves,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    SearchStats* stats = nullptr,
    bool allow_deep_checked_fallback = true,
    const FallbackControl* control = nullptr,
    bool permit_overdue_shallow = false,
    bool reject_hanging_major_checks = false) noexcept {
    if (stats != nullptr) {
        ++stats->short_fallback_invocations;
    }
    const PositionFeatures root_features = root.position_features();
    MoveMetadataList fallback_moves = legal_moves;
    if (root.in_check()) {
        // Checked roots must compare all king evasions before spending the
        // bounded fallback budget on unrelated interpositions. Otherwise a
        // slow first candidate can leave a later, safer king escape unseen.
        std::stable_partition(
            fallback_moves.begin(), fallback_moves.end(),
            [](const MoveMetadata& metadata) { return metadata.moving_piece == PieceType::king; });
    }
    std::optional<Move> best_move;
    int best_score = -kInfinity;
    int best_king_escape_risk = kInfinity;
    bool best_is_king_evasion = false;
    bool best_king_evasion_is_capture = false;
    bool best_is_safe_capture = false;
    bool best_is_safe_forcing_capture = false;
    bool best_is_safe_equal_non_pawn_exchange = false;
    bool best_is_forcing_check = false;
    bool best_is_hanging_major_check = false;
    std::optional<Move> best_quiet_forcing_move;
    int best_quiet_forcing_score = -kInfinity;
    std::optional<Move> broad_check_fallback_move;
    int broad_check_fallback_score = -kInfinity;
    bool broad_check_fallback_is_major = false;
    std::size_t quiet_forcing_researches = 0;
    std::optional<Move> unsafe_best_move;
    int unsafe_best_score = -kInfinity;
    bool unsafe_best_is_quiet = false;
    std::optional<Move> first_unsafe_quiet_move;
    int first_unsafe_quiet_score = -kInfinity;
    std::optional<Move> best_safe_pawn_capture;
    int best_safe_pawn_capture_score = -kInfinity;
    try {
        for (const MoveMetadata& metadata : fallback_moves) {
            const bool deadline_seen = control != nullptr && control->interrupted();
            if (deadline_seen && !permit_overdue_shallow) {
                break;
            }
            if (stats != nullptr) {
                ++stats->short_fallback_candidates;
                if (deadline_seen) {
                    ++stats->short_fallback_overdue_candidates;
                }
            }
            const bool perform_deep_fallback = allow_deep_checked_fallback && !deadline_seen;
            const bool bounded_shallow_fallback = permit_overdue_shallow &&
                !allow_deep_checked_fallback;
            GameState after_move = root;
            if (!after_move.make_search_move(metadata)) {
                continue;
            }

            int score = 0;
            const auto move_terminal = fallback_terminal_or_draw_score(
                after_move, root.side_to_move(), 1);
            const bool move_is_terminal = move_terminal.has_value();
            bool hanging_major_check = false;
            if (move_is_terminal) {
                // A fallback move that reaches a terminal position or a
                // forced rule draw must not be replaced by a static
                // evaluation. Claimable draws remain live and are handled as
                // an optional zero score by the continuation helpers. The
                // helper checks mate before draw so an immediate checkmate
                // remains decisive.
                score = *move_terminal;
            } else if (after_move.in_check()) {
                const std::vector<Move> evasions = after_move.legal_moves();
                if (evasions.empty()) {
                    score = kMateScore - 1;
                } else if (perform_deep_fallback) {
                    score = checked_root_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                } else {
                    // A post-deadline shallow pass is intentionally bounded to
                    // the legal evasions of this checking move.  It is still
                // needed to distinguish a useful checking candidate from
                // a hanging checker, so do not make the already-approved
                // overdue shallow pass return -infinity immediately.
                    const FallbackControl* shallow_control =
                        permit_overdue_shallow &&
                            (deadline_seen || bounded_shallow_fallback) ? nullptr : control;
                    score = shallow_checked_root_fallback_score(
                        after_move, metadata, root.side_to_move(), evaluator, evaluator_mutex,
                        shallow_control);
                }
                hanging_major_check = hanging_major_piece_check(after_move, metadata);
                if (reject_hanging_major_checks && hanging_major_check && !metadata.is_capture()) {
                    // The emergency fallback must not rank a capturable
                    // queen/rook check above a quiet move.  The completion
                    // boundary rejects this candidate later, which would
                    // otherwise leave the original shallow move in place.
                    continue;
                }
            } else if (root.in_check()) {
                // A non-checking evasion must be judged against the opponent's
                // immediate forcing replies. Static material alone can prefer
                // a legal-looking king/rook move that leaves a decisive check.
                if (permit_overdue_shallow &&
                    (deadline_seen || bounded_shallow_fallback)) {
                    // Even in the ultra-short checked-root path, use the
                    // bounded reply scanners. Static evaluation alone can
                    // rank a king move into a checking net above the safe
                    // escape; these helpers inspect only forcing replies and
                    // remain independent of the expired time token.
                    score = metadata.moving_piece == PieceType::king ?
                        king_evasion_fallback_score(
                            after_move, root.side_to_move(), evaluator, evaluator_mutex,
                            metadata.is_capture(), nullptr) :
                        root_evasion_fallback_score(
                            after_move, root.side_to_move(), evaluator, evaluator_mutex, nullptr);
                } else if (metadata.moving_piece == PieceType::king) {
                    score = king_evasion_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex,
                        metadata.is_capture(), control);
                } else {
                    score = root_evasion_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                }
            } else {
                score = short_fallback_evaluate(
                    evaluator, after_move, root.side_to_move(), evaluator_mutex);
            }

            // A bounded reply scanner can observe the deadline after this
            // root candidate has already been made. Its -kInfinity result is
            // an interruption sentinel, not a real evaluation; never let it
            // become the authoritative score or feed the fallback tie-breaks.
            if (score <= -kInfinity) {
                if (control != nullptr && control->interrupted()) {
                    break;
                }
                continue;
            }

            // A static root score can overvalue a material-winning move that
            // opens an immediate checking reply.  This matters only when the
            // first search iteration is interrupted and this emergency
            // fallback is authoritative, so spend the bounded extra work on
            // checking replies rather than allowing a one-ply horizon trap.
            bool exposes_immediate_check = false;
            std::size_t forcing_reply_count = 0;
            std::size_t check_evasion_count = 0;
            bool consequence_scan_incomplete = false;
            if (!move_is_terminal && !after_move.in_check() && perform_deep_fallback &&
                !(root.in_check() && metadata.moving_piece == PieceType::king)) {
                MoveMetadataList opponent_moves;
                after_move.legal_moves_with_metadata(
                    opponent_moves, true, true, CheckFlagMode::all_moves);
                if (quiet_move_leaves_safe_capture(metadata, opponent_moves)) {
                    // Do not let the one-ply score of a quiet move with a
                    // safe forcing capture survive the emergency scan.
                    continue;
                }
                for (const MoveMetadata& opponent_move : opponent_moves) {
                    if (control != nullptr && control->interrupted()) {
                        consequence_scan_incomplete = true;
                        break;
                    }
                    const bool forcing_reply = opponent_move.gives_check ||
                        opponent_move.is_capture() ||
                        opponent_move.move.promotion() != Promotion::none;
                    if (!forcing_reply) {
                        continue;
                    }
                    ++forcing_reply_count;
                    if (opponent_move.gives_check) {
                        exposes_immediate_check = true;
                    }
                    GameState after_reply = after_move;
                    if (!after_reply.make_search_move(opponent_move)) {
                        continue;
                    }
                    const int forcing_score = opponent_move.gives_check ?
                        checked_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex, control) :
                        forcing_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex,
                            opponent_move.captured_piece, control);
                    if (forcing_score <= -kInfinity) {
                        consequence_scan_incomplete = true;
                        break;
                    }
                    score = std::min(score, forcing_score);
                }
                if (consequence_scan_incomplete) {
                    // A deadline can fire after one or more opponent replies
                    // have been scored. Those partial minima are not a
                    // reliable comparison between root moves; keep only the
                    // static score when the consequence scan did not finish.
                    score = short_fallback_evaluate(
                        evaluator, after_move, root.side_to_move(), evaluator_mutex);
                }
            } else if (!move_is_terminal && !after_move.in_check() && permit_overdue_shallow &&
                       (deadline_seen || !allow_deep_checked_fallback)) {
                // Once the deadline has fired, inspect one opponent forcing
                // reply and only our forcing continuations. This catches a
                // poisoned capture or immediate check without starting the
                // recursive reply tree used by the normal emergency scan. A
                // caller that explicitly disables deep fallback uses the same
                // bounded scan even if the deadline has not flipped yet, so
                // timing between the root iteration and correction cannot
                // change the tactical safety decision.
                MoveMetadataList opponent_moves;
                after_move.legal_moves_with_metadata(
                    opponent_moves, true, true, CheckFlagMode::all_moves);
                if (!(root.in_check() && metadata.moving_piece == PieceType::king) &&
                    quiet_move_leaves_safe_capture(metadata, opponent_moves)) {
                    // The overdue path is allowed to be conservative: a
                    // quiet move that immediately leaves a safe forcing
                    // capture cannot be its authoritative choice.
                    continue;
                }
                const auto is_forcing_reply = [](const MoveMetadata& move) {
                    return move.gives_check || move.is_capture() ||
                        move.move.promotion() != Promotion::none;
                };
                // Keep the overdue probe bounded, but do not let an early
                // material capture consume the only non-check slot.  A
                // poisoned move can leave both a valuable recapture and a
                // less relevant major capture in the reply list; inspect two
                // valuable captures/promotions so the emergency comparison
                // can see both tactical consequences.
                std::array<const MoveMetadata*, 6> selected_replies{
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
                std::size_t selected_reply_count = 0;
                const auto append_reply = [&](const auto& predicate,
                                              const bool prefer_distinct_source = false) {
                    if (selected_reply_count >= selected_replies.size()) {
                        return;
                    }
                    auto selected = std::find_if(
                        opponent_moves.begin(), opponent_moves.end(),
                        [&](const MoveMetadata& move) {
                            if (std::find(selected_replies.begin(),
                                          selected_replies.begin() + selected_reply_count,
                                          &move) != selected_replies.begin() + selected_reply_count) {
                                return false;
                            }
                            if (prefer_distinct_source && std::any_of(
                                    selected_replies.begin(),
                                    selected_replies.begin() + selected_reply_count,
                                    [&](const MoveMetadata* prior) {
                                        return prior != nullptr &&
                                            prior->gives_check &&
                                            prior->move.from() == move.move.from();
                                    })) {
                                return false;
                            }
                            return predicate(move);
                        });
                    if (selected == opponent_moves.end() && prefer_distinct_source) {
                        // If all checks come from one source, retain the
                        // bounded second probe rather than dropping it.
                        selected = std::find_if(
                            opponent_moves.begin(), opponent_moves.end(),
                            [&](const MoveMetadata& move) {
                                if (std::find(
                                        selected_replies.begin(),
                                        selected_replies.begin() + selected_reply_count,
                                        &move) != selected_replies.begin() + selected_reply_count) {
                                    return false;
                                }
                                return predicate(move);
                            });
                    }
                    if (selected != opponent_moves.end()) {
                        selected_replies[selected_reply_count++] = &*selected;
                    }
                };
                // A material capture can be poisoned by an immediate
                // recapture on the destination square. Probe that concrete
                // consequence before the broader checking/capture probes;
                // otherwise the bounded overdue scan can spend its entire
                // budget on unrelated forcing moves and accept a horizon
                // capture that the opponent simply takes back.
                append_reply([&metadata](const MoveMetadata& move) {
                    return move.is_capture() && move.move.to() == metadata.move.to();
                });
                append_reply([](const MoveMetadata& move) { return move.gives_check; });
                append_reply([](const MoveMetadata& move) { return move.gives_check; }, true);
                append_reply([](const MoveMetadata& move) { return move.gives_check; }, true);
                append_reply([](const MoveMetadata& move) {
                    return (move.is_capture() && move.captured_piece != PieceType::pawn) ||
                        move.move.promotion() != Promotion::none;
                });
                append_reply([](const MoveMetadata& move) {
                    return (move.is_capture() && move.captured_piece != PieceType::pawn) ||
                        move.move.promotion() != Promotion::none;
                });
                if (selected_reply_count == 0) {
                    append_reply(is_forcing_reply);
                }
                for (std::size_t reply_index = 0;
                     reply_index < selected_reply_count; ++reply_index) {
                    const MoveMetadata& opponent_move = *selected_replies[reply_index];
                    ++forcing_reply_count;
                    exposes_immediate_check = exposes_immediate_check ||
                        opponent_move.gives_check;
                    GameState after_reply = after_move;
                    if (!after_reply.make_search_move(opponent_move)) {
                        continue;
                    }
                    const int forcing_score = opponent_move.gives_check ?
                        overdue_check_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex) :
                        forcing_reply_fallback_score(
                            after_reply, root.side_to_move(), evaluator, evaluator_mutex,
                            opponent_move.captured_piece);
                    score = std::min(score, forcing_score);
                }
            }

            // When no root iteration completes, a raw static score can make a
            // losing capture look attractive because it collects material
            // before the recapture is seen.  Keep the emergency fallback
            // conservative: only an immediate checking capture may override
            // SEE's losing-capture warning at this boundary.  Normal completed
            // search iterations still evaluate these moves through qsearch.
            if (metadata.is_capture() && metadata.see_computed &&
                metadata.see_score < 0 && !metadata.gives_check) {
                continue;
            }

            const bool unsafe_pawn_capture = metadata.is_capture() &&
                metadata.captured_piece == PieceType::pawn &&
                (forcing_reply_count > 1 || exposes_immediate_check) &&
                !metadata.gives_check;
            if (unsafe_pawn_capture) {
                if (metadata.see_computed && metadata.see_score >= 0 &&
                    score > best_safe_pawn_capture_score) {
                    // Keep a positive-SEE pawn capture available as a last
                    // resort. It remains excluded from ordinary emergency
                    // ranking, but if every quiet alternative exposes a
                    // forcing reply, choosing the strongest non-losing
                    // capture is safer than returning the least-bad unsafe
                    // quiet move.
                    best_safe_pawn_capture = metadata.move;
                    best_safe_pawn_capture_score = score;
                }
                // A positive one-ply SEE is not enough to make a pawn grab
                // authoritative in an interrupted root.  If the capture
                // leaves multiple forcing replies, or an immediate check that
                // was discovered after the deadline, let a quieter candidate
                // survive the emergency comparison. Narrow pawn captures
                // with no checking consequence remain eligible.
                continue;
            }

            if (after_move.in_check()) {
                check_evasion_count = after_move.legal_moves().size();
            }
            const std::optional<int> king_escape_risk =
                emergency_king_escape_risk(root, metadata, after_move);
            const bool preserve_king_evasion_tie = root.in_check() &&
                best_is_king_evasion && !king_escape_risk.has_value() &&
                score <= best_score + kEmergencyKingEscapeSafetyMargin;
            const bool prefer_quiet_king_evasion = root.in_check() &&
                best_is_king_evasion && best_king_evasion_is_capture &&
                king_escape_risk.has_value() && !metadata.is_capture() &&
                score >= best_score - kEmergencyQuietForcingMargin;
            const bool preserve_quiet_king_evasion = root.in_check() &&
                best_is_king_evasion && !best_king_evasion_is_capture &&
                !best_is_safe_capture && metadata.is_capture() &&
                score <= best_score + kEmergencyQuietForcingMargin;
            const bool safe_capture = metadata.is_capture() && metadata.see_computed &&
                metadata.see_score >= 0;
            // A non-negative SEE on a pawn capture is not enough evidence to
            // protect it from a quiet alternative in the interrupted-root
            // fallback.  Poisoned pawn captures can still expose a forcing
            // recapture or a tactical counterattack that the shallow SEE
            // exchange does not model.  Reserve the safety tie-break for
            // captures of a non-pawn piece, or pawn captures with a narrow
            // forcing-reply set; the normal search and qsearch continue to
            // evaluate every capture normally.
            const bool safe_material_capture = safe_capture &&
                (metadata.captured_piece != PieceType::pawn || forcing_reply_count <= 1);
            const bool safe_forcing_capture = safe_capture && after_move.in_check();
            // A broad check can still be a horizon trap when the checked side
            // has several useful evasions.  Only narrow checks retain the
            // emergency near-tie priority; ordinary search ordering remains
            // unchanged.
            const bool forcing_check = after_move.in_check() &&
                check_evasion_count <= kMaximumEmergencyForcingCheckEvasions;
            const bool broad_quiet_check = after_move.in_check() &&
                !forcing_check && !metadata.is_capture();
            const bool safer_king_evasion = king_escape_risk.has_value() &&
                best_is_king_evasion && score >= best_score - kEmergencyKingEscapeSafetyMargin &&
                *king_escape_risk < best_king_escape_risk;
            const int quiet_safety_tie_margin = best_is_safe_capture ?
                kEmergencySafeExchangeTieMargin : kEmergencySafeCaptureTieMargin;
            const bool quiet_move_tie = !metadata.is_capture() && !safe_capture &&
                best_is_safe_capture &&
                score <= best_score + quiet_safety_tie_margin;
            const bool quiet_forcing_check_tie = !metadata.is_capture() &&
                !forcing_check && best_is_forcing_check &&
                score <= best_score + kEmergencyQuietForcingMargin;
            const bool safe_capture_tie = safe_forcing_capture &&
                !best_is_safe_capture &&
                score >= best_score - kEmergencySafeCaptureTieMargin;
            const bool safe_capture_safety_tie = safe_capture &&
                safe_material_capture && !safe_forcing_capture && !best_is_safe_capture &&
                score >= best_score - kEmergencySafeExchangeTieMargin;
            const bool forcing_check_tie = forcing_check && !best_is_forcing_check &&
                score >= best_score - kEmergencyQuietForcingMargin;
            const bool preserve_safe_forcing_capture = best_is_safe_forcing_capture &&
                !metadata.is_capture() && metadata.gives_check &&
                score <= best_score + kEmergencyQuietForcingMargin;

            // An equal-value, non-pawn exchange with non-negative SEE has
            // already passed a material safety check and its forcing replies
            // were included above. Do not discard that tactical exchange only
            // because the opponent also has a checking reply; keep the broad
            // immediate-check guard for quiet moves and pawn captures.
            const bool safe_equal_non_pawn_exchange =
                is_safe_equal_non_pawn_exchange(metadata);
            const bool safe_non_pawn_capture = safe_capture &&
                (metadata.captured_piece == PieceType::queen ||
                 (metadata.moving_piece == PieceType::pawn &&
                  (metadata.captured_piece == PieceType::bishop ||
                   metadata.captured_piece == PieceType::knight)));
            const bool safe_equal_exchange_tie = safe_equal_non_pawn_exchange &&
                !best_is_safe_capture && !best_is_safe_forcing_capture &&
                !best_is_safe_equal_non_pawn_exchange &&
                score >= best_score - kEmergencySafeExchangeTieMargin;


            if (broad_quiet_check) {
                // Keep a broad, non-hanging major-piece check as a deferred
                // forcing candidate.  Minor-piece and pawn checks with a
                // broad evasion set are too easy to overvalue at this shallow
                // horizon, while a queen/rook check can still be the critical
                // forcing resource an interrupted root needs.
                const bool major_check = metadata.moving_piece == PieceType::queen ||
                    metadata.moving_piece == PieceType::rook;
                if (major_check && !hanging_major_check &&
                    (!broad_check_fallback_move.has_value() ||
                    score > broad_check_fallback_score)) {
                    broad_check_fallback_move = metadata.move;
                    broad_check_fallback_score = score;
                    broad_check_fallback_is_major = true;
                }
                continue;
            }
            if (exposes_immediate_check && !root.in_check() &&
                !safe_equal_non_pawn_exchange && !safe_non_pawn_capture) {
                const int unsafe_score = score -
                    (metadata.moving_piece == PieceType::pawn && !metadata.is_capture() ?
                         kEmergencyUnsafePawnQuietPenalty : 0);
                if (!first_unsafe_quiet_move.has_value() && !metadata.is_capture() &&
                    metadata.move.promotion() == Promotion::none) {
                    first_unsafe_quiet_move = metadata.move;
                    first_unsafe_quiet_score = unsafe_score;
                }
                if (!unsafe_best_move.has_value() || unsafe_score > unsafe_best_score) {
                    unsafe_best_score = unsafe_score;
                    unsafe_best_move = metadata.move;
                    unsafe_best_is_quiet = !metadata.is_capture() &&
                        metadata.move.promotion() == Promotion::none;
                }
                continue;
            }

            const bool quiet_forcing = perform_deep_fallback && !root.in_check() &&
                !metadata.is_capture() &&
                !metadata.gives_check && metadata.move.promotion() == Promotion::none &&
                quiet_move_is_forcing(root_features, after_move.position_features(), metadata);
            if (quiet_forcing &&
                quiet_forcing_researches < kMaximumEmergencyQuietForcingResearches) {
                ++quiet_forcing_researches;
                if (stats != nullptr) {
                    // The emergency path performs the same bounded quiet-forcing
                    // consequence extension as the normal root search, but may
                    // run before a complete negamax iteration fits in the clock.
                    ++stats->quiet_forcing_extensions;
                }
                const int consequence_score = quiet_forcing_fallback_score(
                    after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                if (consequence_score > -kInfinity) {
                    score = consequence_score;
                }
            }
            if (quiet_forcing &&
                (!best_quiet_forcing_move.has_value() || score > best_quiet_forcing_score)) {
                best_quiet_forcing_score = score;
                best_quiet_forcing_move = metadata.move;
            }
            if ((!best_move.has_value() || score > best_score || safer_king_evasion ||
                 safe_capture_tie || safe_capture_safety_tie || safe_equal_exchange_tie ||
                 forcing_check_tie || prefer_quiet_king_evasion) &&
                (!quiet_move_tie || prefer_quiet_king_evasion) &&
                !quiet_forcing_check_tie &&
                !preserve_safe_forcing_capture &&
                !preserve_king_evasion_tie &&
                !preserve_quiet_king_evasion) {
                best_score = score;
                best_move = metadata.move;
                best_is_king_evasion = king_escape_risk.has_value();
                best_king_evasion_is_capture = best_is_king_evasion && metadata.is_capture();
                best_king_escape_risk = king_escape_risk.value_or(kInfinity);
                best_is_safe_capture = safe_material_capture;
                best_is_safe_forcing_capture = safe_forcing_capture;
                best_is_safe_equal_non_pawn_exchange = safe_equal_non_pawn_exchange;
                best_is_forcing_check = forcing_check;
                best_is_hanging_major_check = hanging_major_check;
            }
        }
    } catch (...) {
        return std::nullopt;
    }
    if (best_quiet_forcing_move.has_value() &&
        !best_is_safe_capture &&
        (!best_is_safe_equal_non_pawn_exchange ||
         best_quiet_forcing_score > best_score + kEmergencySafeExchangeTieMargin) &&
        (!best_is_forcing_check ||
         best_quiet_forcing_score > best_score + kEmergencyQuietForcingMargin) &&
         (!best_move.has_value() ||
          best_quiet_forcing_score >= best_score - kEmergencyQuietForcingMargin)) {
        return ShortFallbackChoice{*best_quiet_forcing_move, best_quiet_forcing_score};
    }
    const bool broad_check_can_replace_competitive_move =
        broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
        best_move.has_value() &&
        !best_is_safe_capture && !best_is_safe_forcing_capture &&
        !best_is_safe_equal_non_pawn_exchange &&
        broad_check_fallback_score >= best_score - kEmergencyBroadCheckReplacementMargin;
    if (broad_check_can_replace_competitive_move ||
        (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
         !best_move.has_value() &&
         (!unsafe_best_move.has_value() ||
         broad_check_fallback_score >= unsafe_best_score -
              kEmergencyBroadCheckReplacementMargin))) {
        return ShortFallbackChoice{*broad_check_fallback_move, broad_check_fallback_score};
    }
    if (!best_move.has_value() && best_safe_pawn_capture.has_value() &&
        first_unsafe_quiet_move.has_value()) {
        // If every positive-SEE pawn capture was quarantined and all useful
        // quiet alternatives expose immediate checks, retain stable move
        // ordering instead of promoting the least-bad unsafe quiet score.
        return ShortFallbackChoice{*first_unsafe_quiet_move, first_unsafe_quiet_score};
    }
    if (unsafe_best_move.has_value() &&
        (unsafe_best_is_quiet || best_is_hanging_major_check) &&
        (!best_move.has_value() ||
         unsafe_best_score >= best_score + kEmergencyQuietForcingMargin)) {
        return ShortFallbackChoice{*unsafe_best_move, unsafe_best_score};
    }
    if (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
        !best_move.has_value()) {
        return ShortFallbackChoice{*broad_check_fallback_move, broad_check_fallback_score};
    }
    if (best_move.has_value()) {
        return ShortFallbackChoice{*best_move, best_score};
    }
    if (unsafe_best_move.has_value()) {
        return ShortFallbackChoice{*unsafe_best_move, unsafe_best_score};
    }
    return std::nullopt;
}

bool short_fallback_should_replace_completed_root(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& current_move, const Move fallback_move) noexcept {
    try {
        const auto metadata_for = [&legal_moves](const Move move) {
            return std::find_if(
                legal_moves.begin(), legal_moves.end(),
                [move](const MoveMetadata& metadata) { return metadata.move == move; });
        };
        const auto fallback_metadata = metadata_for(fallback_move);
        if (fallback_metadata == legal_moves.end()) {
            return false;
        }
        if (fallback_metadata->gives_check && !fallback_metadata->is_capture() &&
            (fallback_metadata->moving_piece == PieceType::queen ||
             fallback_metadata->moving_piece == PieceType::rook)) {
            GameState after_fallback = root;
            if (after_fallback.make_search_move(*fallback_metadata) &&
                hanging_major_piece_check(after_fallback, *fallback_metadata)) {
                // Do not let a shallow emergency correction overturn a
                // completed root iteration with a capturable major-piece
                // check. The ordinary search/qsearch must establish such a
                // sacrifice as sound before it becomes authoritative.
                return false;
            }
        }
        const bool fallback_forcing = fallback_metadata->is_capture() ||
            fallback_metadata->gives_check ||
            fallback_metadata->move.promotion() != Promotion::none;
        if (!current_move.has_value()) {
            return true;
        }

        const auto current_metadata = metadata_for(*current_move);
        if (current_metadata == legal_moves.end()) {
            return false;
        }
        if (fallback_forcing) {
            if (current_metadata->gives_check && !current_metadata->is_capture() &&
                current_metadata->move.promotion() == Promotion::none) {
                GameState after_current = root;
                if (after_current.make_search_move(*current_metadata)) {
                    const std::size_t evasion_count = after_current.legal_moves().size();
                    if (evasion_count == 0) {
                        // Do not replace a completed mate with any shallow
                        // alternative, even when that alternative captures.
                        return false;
                    }
                    if (fallback_metadata->is_capture()) {
                        // A completed non-mating checking move that can be
                        // superseded by a checking capture has a concrete
                        // material/tactical alternative worth keeping in the
                        // bounded correction.
                        return true;
                    }
                    if (!hanging_major_piece_check(after_current, *current_metadata)) {
                        // A completed non-hanging check is already a forcing
                        // result; another shallow non-capturing check alone is
                        // not enough evidence to replace it.
                        return false;
                    }
                }
            }
            return true;
        }
        if (root.in_check() &&
            current_metadata->moving_piece == PieceType::king &&
            fallback_metadata->moving_piece == PieceType::king) {
            // A depth-one checked-root result can choose one legal king
            // escape before the bounded forcing-reply scanner sees the
            // difference between two quiet evasions.  Let the scanner's
            // complete king-evasion comparison replace that result; unlike
            // ordinary quiet moves, both candidates are legal responses to
            // the same check and the fallback has already inspected their
            // immediate forcing consequences.
            return fallback_move != *current_move;
        }
        if (current_metadata->is_capture()) {
            const bool current_exposes = root_move_exposes_immediate_check(
                root, *current_metadata);
            if (current_metadata->captured_piece == PieceType::pawn && current_exposes) {
                // A completed depth-one capture is normally protected from a
                // quiet emergency replacement.  A pawn grab that leaves an
                // immediate checking reply is the exception: the bounded
                // fallback has already scored those replies, so permit only
                // a quiet, non-promoting alternative.  Several moves in a
                // tactical root can expose the same checking resource; a
                // binary exposure test must not reject every alternative.
                return !fallback_metadata->is_capture() &&
                    !fallback_metadata->gives_check &&
                    fallback_metadata->move.promotion() == Promotion::none;
            }
            return true;
        }
        const bool fallback_is_king_escape = root.in_check() &&
            fallback_metadata->moving_piece == PieceType::king;
        return fallback_is_king_escape &&
            root_move_exposes_immediate_check(root, *current_metadata);
    } catch (...) {
        return false;
    }
}

bool short_capture_needs_fallback(const MoveMetadataList& legal_moves,
                                  const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    return metadata != legal_moves.end() && metadata->is_capture() &&
        !metadata->gives_check;
}

bool short_check_needs_fallback(const MoveMetadataList& legal_moves,
                                const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (metadata == legal_moves.end() || metadata->is_capture() ||
        !metadata->gives_check || metadata->move.promotion() != Promotion::none) {
        return false;
    }
    // A non-capturing queen/rook check can be attractive at depth one while
    // the checked side still has a quiet escape and the checking piece has no
    // immediate material consequence. Reuse the bounded short fallback for
    // this narrow class; minor-piece and pawn checks remain on the ordinary
    // completed-iteration path.
    return metadata->moving_piece == PieceType::queen ||
        metadata->moving_piece == PieceType::rook;
}

bool is_safe_short_capture(const MoveMetadata& metadata) noexcept {
    return metadata.is_capture() && metadata.see_computed && metadata.see_score >= 0;
}

bool is_safe_material_capture(const MoveMetadata& metadata) noexcept {
    return is_safe_short_capture(metadata) &&
        metadata.captured_piece != PieceType::pawn;
}

bool is_safe_equal_non_pawn_exchange(const MoveMetadata& metadata) noexcept {
    return is_safe_short_capture(metadata) && metadata.moving_piece != PieceType::pawn &&
        metadata.captured_piece == metadata.moving_piece &&
        metadata.captured_piece != PieceType::none;
}

bool short_quiet_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }

    const auto candidate_metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (candidate_metadata == legal_moves.end() || candidate_metadata->is_capture() ||
        candidate_metadata->gives_check ||
        candidate_metadata->move.promotion() != Promotion::none) {
        return false;
    }

    return std::any_of(legal_moves.begin(), legal_moves.end(),
                       [candidate, &root](const MoveMetadata& move) {
                           return move.move != *candidate &&
                               (is_safe_material_capture(move) ||
                                (move.is_capture() &&
                                 (move.gives_check || root.move_gives_check(move.move))));
                       });
}

bool short_quiet_exposes_immediate_check(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    if (metadata == legal_moves.end() || metadata->is_capture() ||
        metadata->gives_check || metadata->move.promotion() != Promotion::none) {
        return false;
    }
    return root_move_exposes_immediate_check(root, *metadata);
}

bool short_checked_root_king_evasion_needs_fallback(
    const GameState& root, const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept {
    if (!root.in_check() || !candidate.has_value()) {
        return false;
    }
    const auto metadata = std::find_if(
        legal_moves.begin(), legal_moves.end(),
        [&candidate](const MoveMetadata& move) { return move.move == *candidate; });
    return metadata != legal_moves.end() && metadata->moving_piece == PieceType::king;
}

bool quiet_move_leaves_safe_capture(
    const MoveMetadata& candidate, const MoveMetadataList& opponent_moves) noexcept {
    if (candidate.is_capture() || candidate.gives_check ||
        candidate.move.promotion() != Promotion::none) {
        return false;
    }
    return std::any_of(opponent_moves.begin(), opponent_moves.end(),
                       [&candidate](const MoveMetadata& opponent_move) {
                           if (!opponent_move.is_capture() || !opponent_move.see_computed ||
                               opponent_move.see_score < 0) {
                               return false;
                           }
                           return opponent_move.move.to() == candidate.move.to() ||
                               opponent_move.captured_piece != PieceType::pawn;
                       });
}

// Root workers consume a shared atomic index.  At a short time limit the
// order of that queue is therefore part of playing strength: moves that were
// strong at the preceding completed iteration should be handed to workers
// before previously unseen tail moves.  Keep the identity separate from the
// current vector position so reordering does not change deterministic ties.
struct RootScheduleRecord {
    Move move = Move::no_move();
    std::size_t stable_index = 0;
    int previous_score = -kInfinity;
    bool has_previous_score = false;
    bool previous_exact = false;
};

// Lazy SMP helper pool. The main thread keeps running the authoritative
// iterative deepening; each helper searches the same root with a depth offset
// so it can deposit lines into the shared transposition table that the main
// thread has not reached yet. Helper results are never published -- only their
// statistics are merged once the search finishes. The pool only exists for
// Threads > 1 without a node limit, so the single-threaded path stays
// bit-for-bit deterministic.
class LazySmpPool {
public:
    LazySmpPool(std::size_t helper_count, const GameState& root, MoveMetadataList root_moves,
                const Evaluator& evaluator, TranspositionTable& table, TimeManager& time_manager,
                std::atomic_bool& stop_requested, std::mutex* evaluator_mutex,
                bool use_transposition_table,
                SearchOptions::QuietHistorySideHook quiet_history_side_hook,
                TablebaseSearchBinding tablebase_binding = {})
        : root_(root), root_moves_(std::move(root_moves)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          quiet_history_side_hook_(std::move(quiet_history_side_hook)),
          tablebase_binding_(std::move(tablebase_binding)),
          helper_stats_(std::max<std::size_t>(1, helper_count)) {
        helpers_.reserve(helper_stats_.size());
        try {
            for (std::size_t index = 0; index < helper_stats_.size(); ++index) {
                helpers_.emplace_back(&LazySmpPool::helper_loop, this, index);
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    LazySmpPool(const LazySmpPool&) = delete;
    LazySmpPool& operator=(const LazySmpPool&) = delete;

    ~LazySmpPool() {
        stop();
    }

    void stop() noexcept {
        stopping_.store(true, std::memory_order_relaxed);
        helper_abort_.store(true, std::memory_order_relaxed);
        for (std::thread& helper : helpers_) {
            if (helper.joinable()) {
                helper.join();
            }
        }
    }

    [[nodiscard]] SearchStats stats() const noexcept {
        SearchStats total{};
        for (const SearchStats& helper : helper_stats_) {
            accumulate_stats(total, helper);
        }
        return total;
    }

private:
    void helper_loop(std::size_t helper_index) {
        // A helper owns its own position copy, ordering tables and evaluator
        // worker; only the transposition table and the time budget are shared.
        // Staggered starting depths keep the helpers from duplicating each
        // other's trees.
        GameState state = root_;
        auto context_storage = std::make_unique<SearchContext>(
            evaluator_, table_, time_manager_, stop_requested_, nullptr, evaluator_mutex_,
            &root_moves_, use_transposition_table_, quiet_history_side_hook_, tablebase_binding_);
        SearchContext& context = *context_storage;
        int depth = 1 + static_cast<int>(helper_index % 3);
        while (!stopping_.load(std::memory_order_relaxed) &&
               !stop_requested_.load(std::memory_order_relaxed) &&
               !time_manager_.should_stop(0) && depth <= kMaximumSearchDepth) {
            PrincipalVariation pv;
            context.begin_iteration(&helper_abort_);
            context.negamax(state, depth, -kInfinity, kInfinity, 0, pv);
            accumulate_stats(helper_stats_[helper_index], context.stats);
            if (stopping_.load(std::memory_order_relaxed) || context.aborted) {
                break;
            }
            ++depth;
        }
    }

    GameState root_;
    MoveMetadataList root_moves_;
    const Evaluator& evaluator_;
    TranspositionTable& table_;
    TimeManager& time_manager_;
    std::atomic_bool& stop_requested_;
    std::mutex* evaluator_mutex_ = nullptr;
    bool use_transposition_table_ = true;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook_;
    TablebaseSearchBinding tablebase_binding_;
    std::vector<std::thread> helpers_;
    std::vector<SearchStats> helper_stats_;
    std::atomic_bool stopping_{false};
    std::atomic_bool helper_abort_{false};
};

class RootWorkerPool {
public:
    RootWorkerPool(std::size_t worker_count, const Evaluator& evaluator, TranspositionTable& table,
                   TimeManager& time_manager, std::atomic_bool& stop_requested,
                   std::atomic<std::uint64_t>* global_nodes, std::mutex* evaluator_mutex,
                   bool use_transposition_table,
                   SearchOptions::QuietHistorySideHook quiet_history_side_hook = {},
                   TablebaseSearchBinding tablebase_binding = {})
        : worker_count_(std::max<std::size_t>(1, worker_count)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested), global_nodes_(global_nodes),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          quiet_history_side_hook_(std::move(quiet_history_side_hook)),
          tablebase_binding_(std::move(tablebase_binding)), worker_stats_(worker_count_) {
        workers_.reserve(worker_count_);
        try {
            for (std::size_t index = 0; index < worker_count_; ++index) {
                workers_.emplace_back(&RootWorkerPool::worker_loop, this, index);
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    RootWorkerPool(const RootWorkerPool&) = delete;
    RootWorkerPool& operator=(const RootWorkerPool&) = delete;

    ~RootWorkerPool() {
        shutdown();
    }

    void run(const GameState& root, const MoveMetadataList& root_moves, int depth,
             bool root_check_extension, const std::vector<std::size_t>& stable_root_indices,
             std::vector<RootLine>& lines, bool use_root_pvs, int root_static_eval,
             int window_alpha, int window_beta, SearchStats& stats, bool& aborted,
             bool& window_failed_low, bool& window_failed_high) {
        auto job = std::make_shared<Job>();
        job->root = &root;
        job->root_moves = &root_moves;
        job->depth = depth;
        job->root_check_extension = root_check_extension;
        job->stable_root_indices = &stable_root_indices;
        job->lines = &lines;
        job->worker_stats = &worker_stats_;
        job->use_root_pvs = use_root_pvs;
        job->root_static_eval = root_static_eval;
        job->window_alpha = window_alpha;
        job->window_beta = window_beta;
        job->root_alpha.store(window_alpha, std::memory_order_relaxed);
        if (depth == 1) {
            job->root_features = root.position_features();
        }

        {
            std::lock_guard lock(mutex_);
            ++sequence_;
            job->sequence = sequence_;
            finished_workers_ = 0;
            job_ = job;
        }
        work_available_.notify_all();

        {
            std::unique_lock lock(mutex_);
            work_finished_.wait(lock, [this] { return finished_workers_ == worker_count_; });
        }

        stats = {};
        for (const SearchStats& worker_stats : worker_stats_) {
            accumulate_stats(stats, worker_stats);
        }
        aborted = job->aborted.load(std::memory_order_relaxed) ||
            stop_requested_.load(std::memory_order_relaxed);
        window_failed_high = job->window_failed_high.load(std::memory_order_relaxed);
        window_failed_low = false;
        if (!aborted && !window_failed_high && window_alpha > -kInfinity) {
            const bool has_exact_score_above_alpha = std::any_of(
                lines.begin(), lines.end(), [window_alpha](const RootLine& line) {
                    return line.completed && line.exact && line.score > window_alpha;
                });
            // A selective full-window line may still be useful for ranking,
            // but it cannot certify that the aspiration floor was cleared.
            // Only an exact root line may prevent a fail-low retry.
            window_failed_low = !has_exact_score_above_alpha;
        }
    }

private:
    struct Job {
        const GameState* root = nullptr;
        const MoveMetadataList* root_moves = nullptr;
        int depth = 0;
        bool root_check_extension = false;
        const std::vector<std::size_t>* stable_root_indices = nullptr;
        std::vector<RootLine>* lines = nullptr;
        std::vector<SearchStats>* worker_stats = nullptr;
        std::optional<PositionFeatures> root_features;
        int root_static_eval = 0;
        std::atomic<std::size_t> next_move = 0;
        std::atomic<int> root_alpha = -kInfinity;
        int window_alpha = -kInfinity;
        int window_beta = kInfinity;
        std::atomic_bool window_failed_high = false;
        std::atomic_bool aborted = false;
        bool use_root_pvs = false;
        std::uint64_t sequence = 0;
    };

    void worker_loop(std::size_t worker_index) {
        // Root jobs are authoritative. The striped table keeps concurrent probe/store activity
        // safe without requiring a serial confirmation search.
        auto context_storage = std::make_unique<SearchContext>(
            evaluator_, table_, time_manager_, stop_requested_, global_nodes_,
            evaluator_mutex_, nullptr, use_transposition_table_, quiet_history_side_hook_,
            tablebase_binding_);
        SearchContext& context = *context_storage;
        std::uint64_t seen_sequence = 0;

        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this, seen_sequence] {
                    return stopping_ || (job_ != nullptr && job_->sequence != seen_sequence);
                });
                if (stopping_) {
                    return;
                }
                job = job_;
            }

            seen_sequence = job->sequence;
            context.begin_iteration(&job->aborted);
            // The serial root search initializes ply zero before descending
            // into its children. Root-parallel workers start directly at ply
            // one, so seed the same parent snapshot from the already computed
            // root evaluation; this keeps improving/opponent-worsening and
            // reduction hindsight identical without an extra evaluator call.
            detail::SearchFrame& root_frame = context.stack.frame(0);
            root_frame.static_eval = job->root_static_eval;
            root_frame.static_eval_valid = true;
            root_frame.in_check = job->root->in_check();
            root_frame.previous_move = Move::no_move();
            root_frame.current_move = Move::no_move();
            root_frame.move_count = 0;
            root_frame.reduction = 0;
            root_frame.cutoff_count = 0;
            root_frame.tt_pv = true;

            const auto search_root = [&context, &job](std::size_t move_index) {
                const MoveMetadata& root_move = (*job->root_moves)[move_index];
                try {
                    GameState child = *job->root;
                    if (!child.make_search_move(root_move)) {
                        // Root metadata is generated from the unchanged job
                        // position. A failed application therefore signals a
                        // broken root contract, not a move that may simply be
                        // omitted from an otherwise completed iteration.
                        context.request_abort();
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }

                    PrincipalVariation child_pv;
                    int child_depth = job->depth - 1 +
                        (job->root_check_extension ? 1 : 0);
                    const int child_check_extensions_remaining =
                        job->root_check_extension ? kMaximumCheckExtensionsPerPath - 1 :
                            kMaximumCheckExtensionsPerPath;
                    if (job->depth == 1 && job->root_features.has_value() &&
                        !root_move.is_capture() && !root_move.gives_check &&
                        root_move.move.promotion() == Promotion::none &&
                        quiet_move_has_direct_forcing_target(*job->root_features, root_move) &&
                        quiet_move_is_forcing(*job->root_features, child.position_features(),
                                              root_move)) {
                        ++context.stats.quiet_forcing_extensions;
                        ++child_depth;
                    }
                    const int shared_alpha = job->root_alpha.load(std::memory_order_relaxed);
                    const bool scout = job->use_root_pvs && move_index != 0 &&
                        shared_alpha > job->window_alpha;
                    bool exact_score = false;
                    bool completed_score = false;
                    bool child_selective_bound = false;
                    bool child_lower_bound = false;
                    int score_alpha = job->window_alpha;
                    int score = 0;
                    if (scout) {
                        ++context.stats.root_pvs_searches;
                        score_alpha = shared_alpha;
                        bool scout_selective_bound = false;
                        score = -context.negamax(child, child_depth,
                                                 -shared_alpha - 1, -shared_alpha, 1, child_pv,
                                                 root_move.move, true,
                                                 child_check_extensions_remaining,
                                                 Move::no_move(), nullptr,
                                                 &scout_selective_bound, nullptr,
                                                 &child_lower_bound);
                        child_selective_bound = scout_selective_bound;
                        if (!context.aborted && score >= shared_alpha) {
                            ++context.stats.root_pvs_researches;
                            child_pv = {};
                            // A scout that returns exactly the current root
                            // alpha may be an upper-bound hit rather than an
                            // exact tie.  Re-search equality from the full
                            // aspiration floor so deterministic stable-index
                            // ranking sees every genuinely equal line.
                            const int research_alpha = score > shared_alpha ?
                                shared_alpha : job->window_alpha;
                            score_alpha = research_alpha;
                            bool research_selective_bound = false;
                            score = -context.negamax(child, child_depth,
                                                     -job->window_beta, -research_alpha, 1,
                                                     child_pv,
                                                     root_move.move, true,
                                                     child_check_extensions_remaining,
                                                     Move::no_move(), nullptr,
                                                     &research_selective_bound, nullptr,
                                                     &child_lower_bound);
                            child_selective_bound = research_selective_bound;
                            completed_score = !context.aborted &&
                                score > job->window_alpha && score < job->window_beta;
                            exact_score = score > research_alpha &&
                                score < job->window_beta && !child_selective_bound;
                        }
                    } else {
                        score = -context.negamax(child, child_depth,
                                                 -job->window_beta, -job->window_alpha, 1, child_pv,
                                                 root_move.move, true,
                                                 child_check_extensions_remaining,
                                                 Move::no_move(), nullptr,
                                                 &child_selective_bound, nullptr,
                                                 &child_lower_bound);
                        completed_score = !context.aborted &&
                            score > job->window_alpha && score < job->window_beta;
                        exact_score = score > job->window_alpha &&
                            score < job->window_beta && !child_selective_bound;
                    }
                    // A selective child can return a useful score at the
                    // aspiration ceiling without proving that the root move
                    // truly failed high.  Only an authoritative child may
                    // widen the aspiration window; otherwise a selective
                    // estimate can repeatedly drive the root away from the
                    // previous completed score.
                    if (score >= job->window_beta && !child_selective_bound) {
                        job->window_failed_high.store(true, std::memory_order_relaxed);
                    }
                    if (context.aborted) {
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (!child.native_shadow_consistent()) {
                        context.request_abort();
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }

                    int observed_alpha = job->root_alpha.load(std::memory_order_relaxed);
                    while (exact_score && score > observed_alpha &&
                           !job->root_alpha.compare_exchange_weak(
                               observed_alpha, score, std::memory_order_relaxed,
                               std::memory_order_relaxed)) {
                    }

                    RootLine& line = (*job->lines)[move_index];
                    line.searched = true;
                    line.score = score;
                    line.stable_index = (*job->stable_root_indices)[move_index];
                    line.pv.prepend(root_move.move, child_pv);
                    // A scout fail-low is an upper bound for this root move,
                    // not a completed comparable score. A full-window
                    // selective result, however, is still useful when a
                    // full-window fallback must rank otherwise incomplete
                    // root lines. Keep that estimate out of exact aspiration
                    // authority and shared-alpha publication.
                    line.completed = completed_score;
                    line.exact = exact_score;
                    line.selective_bound = child_selective_bound;
                    // A non-selective fail-low is an upper bound for this root
                    // move at the alpha used by its final search. A selective
                    // fail-low is safe only when the child explicitly proved
                    // a lower bound before negation; unresolved/upper-bound
                    // selective paths must block root authority.
                    line.safe_upper_bound = !exact_score && score <= score_alpha &&
                        (!child_selective_bound || child_lower_bound);
                    line.selective_upper_bound = child_selective_bound && child_lower_bound;
                } catch (...) {
                    context.request_abort();
                    job->aborted.store(true, std::memory_order_relaxed);
                }
            };

            while (!job->aborted.load(std::memory_order_relaxed) &&
                   !stop_requested_.load(std::memory_order_relaxed)) {
                const std::size_t move_index = job->next_move.fetch_add(1, std::memory_order_relaxed);
                if (move_index >= job->root_moves->size()) {
                    break;
                }
                search_root(move_index);
            }

            (*job->worker_stats)[worker_index] = context.stats;
            {
                std::lock_guard lock(mutex_);
                ++finished_workers_;
            }
            work_finished_.notify_one();
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    const std::size_t worker_count_;
    const Evaluator& evaluator_;
    TranspositionTable& table_;
    TimeManager& time_manager_;
    std::atomic_bool& stop_requested_;
    std::atomic<std::uint64_t>* global_nodes_;
    std::mutex* evaluator_mutex_;
    bool use_transposition_table_;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook_;
    TablebaseSearchBinding tablebase_binding_;
    std::vector<std::thread> workers_;
    std::vector<SearchStats> worker_stats_;
    std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable work_finished_;
    std::shared_ptr<Job> job_;
    std::size_t finished_workers_ = 0;
    std::uint64_t sequence_ = 0;
    bool stopping_ = false;
};

void safely_report_info(const SearchEventSink& sink, const SearchInfo& info) {
    if (!sink.on_info) {
        return;
    }
    try {
        sink.on_info(info);
    } catch (...) {
    }
}

} // namespace

SearchRunner::SearchRunner(std::shared_ptr<SearchSession> session,
                           std::shared_ptr<const Evaluator> evaluator,
                           std::shared_ptr<TranspositionTable> table,
                           std::shared_ptr<std::mutex> evaluator_mutex,
                           SearchEventSink sink)
    : session_(std::move(session)),
      evaluator_(std::move(evaluator)),
      table_(std::move(table)),
      evaluator_mutex_(std::move(evaluator_mutex)),
      sink_(std::move(sink)) {}

void SearchRunner::run() {
    const auto& session = session_;
    const auto& evaluator = evaluator_;
    const auto& table = table_;
    const auto& evaluator_mutex = evaluator_mutex_;
    const auto& sink = sink_;

    const auto started = std::chrono::steady_clock::now();
    GameState root = session->root();
    // Search never reads the vendored compatibility board. Detaching it keeps
    // interior make/unmake from copying a second board on every node; native
    // legality and keys remain the sole authority. Public GameState consumers
    // (UCI book probing, diagnostics) keep mirror tracking enabled.
    root.detach_mirror();
    // Mutable copy: a UCI ponderhit converts the running search in place and
    // re-arms timing at the top of the next iteration.
    SearchLimits limits = session->limits();
    const SearchOptions& options = session->options();
    gpu::set_gpu_nnue_threaded(options.threads > 1);
    SearchResult result;
    result.identity = session->identity();
    // The orchestrator uses the same table seam as the recursive search layer;
    // nothing below reaches TranspositionTable directly.
    SearchTableAccess table_access(*table);
    try {
        RootTimingContext timing_context;
        timing_context.table_available = table_access.size_mb() != 0;
        if (const auto entry = table_access.probe(search_transposition_key(root), 0);
            entry.has_value()) {
            timing_context.tt_hit = true;
            timing_context.tt_exact = entry->bound == TranspositionBound::exact;
            timing_context.tt_has_best_move = !entry->best_move.is_no_move();
            timing_context.tt_depth = entry->depth;
            timing_context.tt_generation_age = entry->generation_age;
        }

        std::mutex* evaluator_mutex_ptr = evaluator->supports_concurrent_evaluation() ?
            nullptr : evaluator_mutex.get();

        MoveMetadataList all_legal_moves;
        root.legal_moves_with_metadata(
            all_legal_moves, true, true, CheckFlagMode::quiet_moves_only);
        timing_context.legal_move_count = static_cast<std::uint32_t>(all_legal_moves.size());
        timing_context.forcing_move_count = static_cast<std::uint32_t>(std::count_if(
            all_legal_moves.begin(), all_legal_moves.end(), [](const MoveMetadata& metadata) {
                return metadata.is_capture() || metadata.gives_check ||
                    metadata.move.promotion() != Promotion::none;
            }));
        timing_context.in_check = root.in_check();

        MoveMetadataList legal_moves = all_legal_moves;
        if (limits.search_moves_specified) {
            MoveMetadataList filtered_moves;
            for (const MoveMetadata& metadata : legal_moves) {
                if (std::find(limits.search_moves.begin(), limits.search_moves.end(), metadata.move) !=
                    limits.search_moves.end()) {
                    (void)filtered_moves.push_back(metadata);
                }
            }
            legal_moves = filtered_moves;
        }
        table_access.new_generation();
        TimeManager time_manager(limits, root.side_to_move(), options.speed_percent,
                                 options.move_overhead_ms, options.slow_mover_percent,
                                 timing_context);
        const FallbackControl fallback_control{&time_manager, &session->stop_requested()};
        result.timing = time_manager.diagnostics();
        result.best_move = legal_moves.empty() ? std::nullopt :
            std::optional<Move>{legal_moves.front().move};
        if (result.best_move.has_value()) {
            result.pv = {*result.best_move};
        }
        if (evaluator_mutex_ptr != nullptr) {
            std::lock_guard lock(*evaluator_mutex_ptr);
            result.score_cp = evaluator->evaluate(root, root.side_to_move());
        } else {
            result.score_cp = evaluator->evaluate(root, root.side_to_move());
        }
        const int root_static_eval = result.score_cp;
        const bool root_is_claimable_draw = root.is_claimable_draw();
        const bool root_is_forced_draw = root.is_forced_draw();

        // Interior tablebase probing is opt-in, off by default, and mirrors
        // the root probe restrictions except for ponder: a pondering search is
        // converted in place on ponderhit, so the binding must already be in
        // force when the conversion happens.  The diagnostic hook may stand in
        // for a real tablebase so the plumbing can be verified without assets.
        TablebaseSearchBinding tablebase_binding;
        if (options.syzygy_interior_depth > 0 &&
            (options.syzygy != nullptr || options.tablebase_probe_hook) &&
            options.multi_pv == 1 && !options.analyse_mode &&
            !limits.search_moves_specified && !root_is_claimable_draw && !root_is_forced_draw) {
            tablebase_binding.table = options.syzygy ? options.syzygy.get() : nullptr;
            tablebase_binding.interior_depth = options.syzygy_interior_depth;
            tablebase_binding.fifty_move_rule =
                options.syzygy == nullptr || options.syzygy->fifty_move_rule();
            tablebase_binding.probe_hook = options.tablebase_probe_hook;
        }
        const auto normalize_root_fallback_score =
            [root_is_claimable_draw, root_is_forced_draw](const int score) {
                if (root_is_forced_draw) {
                    return 0;
                }
                return root_is_claimable_draw ? std::max(0, score) : score;
            };
        if (root_is_claimable_draw || root_is_forced_draw) {
            // A claim is an available zero-valued root option, but legal
            // continuations may still win. Automatic/dead draws remain
            // terminal; keep both depth-zero fallbacks neutral.
            result.score_cp = std::max(0, result.score_cp);
            if (root_is_forced_draw) {
                result.score_cp = 0;
            }
            result.mate.reset();
        }

        std::optional<SyzygyRootResult> tablebase_result;
        // A ponder search cannot take the root fast path because it must not
        // emit a bestmove before stop/ponderhit.  After an in-place conversion
        // the search continues from its warmed state without a fresh root
        // probe; interior probing, when enabled, is already active.
        if (options.syzygy && options.multi_pv == 1 && !options.analyse_mode &&
            !limits.infinite && !limits.ponder && !limits.search_moves_specified &&
            !root_is_claimable_draw && !root_is_forced_draw &&
            options.syzygy->allows_depth(limits.depth.value_or(1))) {
            std::vector<Move> allowed_moves;
            allowed_moves.reserve(legal_moves.size());
            for (const MoveMetadata& metadata : legal_moves) {
                allowed_moves.push_back(metadata.move);
            }
            tablebase_result = options.syzygy->probe_root(root, allowed_moves);
        }

        const std::optional<std::chrono::milliseconds> effective_time_budget =
            time_manager.time_budget();
        const bool requested_short_timed_fallback = limits.movetime.has_value() &&
            *limits.movetime >= kShortTimedFallbackMinimum &&
            *limits.movetime <= kShortTimedFallbackThreshold;
        const bool ultra_short_nonchecked_fallback = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum &&
            !timing_context.in_check && timing_context.forcing_move_count != 0;
        const bool ultra_short_checked_fallback = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && timing_context.in_check;
        const bool clock_short_timed_fallback = !limits.movetime.has_value() &&
            effective_time_budget.has_value() &&
            *effective_time_budget <= kShortTimedFallbackThreshold;
        bool ultra_short_nonchecked_forcing_root = false;
        if (limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && !timing_context.in_check) {
            const PositionFeatures root_features = root.position_features();
            ultra_short_nonchecked_forcing_root = std::any_of(
                all_legal_moves.begin(), all_legal_moves.end(),
                [&root_features](const MoveMetadata& metadata) {
                    return quiet_move_has_direct_forcing_target(root_features, metadata);
                });
        }
        const bool short_search_fallback = requested_short_timed_fallback ||
            ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root ||
            ultra_short_checked_fallback || clock_short_timed_fallback;
        const bool short_tactical_fallback = short_search_fallback &&
            (timing_context.in_check || timing_context.forcing_move_count != 0 ||
             ultra_short_nonchecked_forcing_root);
        const bool short_tactical_budget =
            (timing_context.in_check || timing_context.forcing_move_count != 0 ||
             ultra_short_nonchecked_forcing_root) &&
            ((limits.movetime.has_value() &&
              *limits.movetime <= kShortTimedSerialThreshold) ||
             (!limits.movetime.has_value() && effective_time_budget.has_value() &&
              *effective_time_budget <= kShortTimedSerialThreshold));
        // A checked root has no time for the full emergency evasion/reply
        // scan when the hard budget is below 150 ms.  Keep the fallback
        // to one static post-evasion evaluation in that band so the
        // authoritative first root iteration still gets a chance to
        // complete. Normal timed and fixed-depth paths retain the deeper
        // checking fallback.
        const bool shallow_checked_fallback = root.in_check() &&
            ((limits.movetime.has_value() &&
              *limits.movetime < kShortTimedFallbackMinimum) ||
             (!limits.movetime.has_value() && effective_time_budget.has_value() &&
              *effective_time_budget < kShortTimedFallbackMinimum));
        const bool allow_deep_checked_fallback = !shallow_checked_fallback;
        const bool defer_expensive_short_fallback = clock_short_timed_fallback;
        const bool ultra_short_nonchecking_parallel = limits.movetime.has_value() &&
            *limits.movetime < kShortTimedFallbackMinimum && !timing_context.in_check;

        // Lazy SMP replaces the root-splitting pool for the common
        // Threads > 1 configuration: the main thread keeps the deterministic
        // root driver and publishes the result, while helpers search the same
        // root against the shared transposition table. Node-limited, MultiPV,
        // forced-draw and short-tactical searches keep the existing
        // root-parallel path because their accounting must stay exact.
        const bool use_lazy_smp = options.threads > 1 && options.multi_pv == 1 &&
            !time_manager.node_limit().has_value() && !short_tactical_budget &&
            !root_is_claimable_draw && !root_is_forced_draw && legal_moves.size() >= 2;
        std::unique_ptr<LazySmpPool> lazy_pool;
        if (use_lazy_smp) {
            lazy_pool = std::make_unique<LazySmpPool>(
                options.threads - 1, root, legal_moves, *evaluator, *table, time_manager,
                session->stop_requested(), evaluator_mutex_ptr, true,
                options.quiet_history_side_hook, tablebase_binding);
        }

        if (legal_moves.empty()) {
            result.score_cp = root.in_check() ? -kMateScore : 0;
            result.mate = mate_from_score(result.score_cp);
        } else if (tablebase_result.has_value()) {
            result.best_move = tablebase_result->moves.front();
            result.pv = {*result.best_move};
            result.score_cp = tablebase_result->score.score_cp;
            result.mate = tablebase_result->score.mate;
            result.completed_depth = 1;
            result.stats.tbhits = 1;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            SearchInfo info{1, result.score_cp, result.mate, 0, 0, elapsed,
                            {result.best_move.value()}, 0, 0, 0, 1, 1};
            info.exact_wdl = syzygy_wdl_permill(tablebase_result->wdl);
            safely_report_info(sink, info);
        } else if (root_is_claimable_draw || root_is_forced_draw || use_lazy_smp ||
                   (options.threads == 1 && options.multi_pv == 1) || legal_moves.size() < 2 ||
                   (time_manager.node_limit().has_value() && options.multi_pv == 1) ||
                   (short_tactical_budget && !ultra_short_nonchecking_parallel)) {
            auto context_storage = std::make_unique<SearchContext>(
                *evaluator, *table, time_manager, session->stop_requested(),
                nullptr, evaluator_mutex_ptr, &legal_moves, true,
                options.quiet_history_side_hook, tablebase_binding);
            SearchContext& context = *context_storage;
            context.allow_root_forcing_extension = limits.depth.has_value() &&
                *limits.depth == 1;
            const bool ultra_short_nonchecked_extension = limits.movetime.has_value() &&
                *limits.movetime < kShortTimedFallbackMinimum &&
                !root.in_check();
            if (requested_short_timed_fallback || ultra_short_nonchecked_extension) {
                context.allow_root_forcing_extension = true;
            }
            context.ordering.order(root, legal_moves, std::nullopt, 0);
            result.best_move = legal_moves.front().move;
            bool used_short_fallback = false;
            // Do not spend an ultra-short non-checking movetime in the
            // emergency evaluator before the first authoritative root
            // iteration. The ordered legal move remains the safety value
            // if the clock expires, while a completed iteration remains
            // authoritative when it fits.
            const bool defer_nonchecking_short_fallback =
                ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root;
            if (short_tactical_fallback && !defer_nonchecking_short_fallback &&
                !ultra_short_checked_fallback && !defer_expensive_short_fallback) {
                if (const auto fallback = short_search_fallback_move(
                        root, legal_moves, *evaluator, evaluator_mutex_ptr,
                        &context.stats, allow_deep_checked_fallback, &fallback_control);
                    fallback.has_value()) {
                    result.best_move = fallback->move;
                    result.pv = {fallback->move};
                    const int fallback_score = normalize_root_fallback_score(fallback->score);
                    result.score_cp = fallback_score;
                    result.mate = mate_from_score(fallback_score);
                    used_short_fallback = true;
                }
            } else if (short_search_fallback && !defer_nonchecking_short_fallback &&
                       !ultra_short_checked_fallback) {
                if (const auto fallback = first_safe_short_search_move(root, legal_moves);
                    fallback.has_value()) {
                    result.best_move = *fallback;
                    result.pv = {*fallback};
                    used_short_fallback = true;
                }
            }

            int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            bool unbounded = limits.infinite || limits.ponder;
            if (limits.ponder && limits.depth.has_value()) {
                // While pondering, a depth limit only says how deep the GUI
                // wanted the search before it started pondering; the engine must
                // keep deepening instead of re-searching the same final depth
                // until stop/ponderhit.  A ponderhit conversion below reinstates
                // the converted depth/time budget.
                maximum_depth = kMaximumSearchDepth;
            }
            std::optional<int> previous_score;
            std::chrono::milliseconds previous_iteration_elapsed{1};
            std::chrono::milliseconds previous_report_elapsed{0};
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                if (auto conversion = session->take_ponderhit_limits(); conversion.has_value()) {
                    limits = std::move(*conversion);
                    time_manager.reconfigure(limits, root.side_to_move(), options.speed_percent,
                                             options.move_overhead_ms, options.slow_mover_percent,
                                             timing_context);
                    maximum_depth = std::min(
                        kMaximumSearchDepth,
                        std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                    unbounded = limits.infinite || limits.ponder;
                }
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                if (!unbounded && !time_manager.should_start_next_iteration(previous_iteration_elapsed)) {
                    break;
                }
                if (context.interrupted()) {
                    break;
                }
                const std::optional<Move> previous_best_move = result.best_move;
                const std::vector<Move> previous_pv = result.pv;
                const bool had_completed_iteration = result.completed_depth > 0;
                context.root_move_hint = had_completed_iteration ? result.best_move : std::nullopt;
                PrincipalVariation pv;
                int alpha = -kInfinity;
                int beta = kInfinity;
                bool aspiration_researched = false;
                int aspiration_window = kAspirationWindow + std::min(32, depth * 2);
                if (previous_score.has_value()) {
                    alpha = std::max(-kInfinity, *previous_score - aspiration_window);
                    beta = std::min(kInfinity, *previous_score + aspiration_window);
                    if (result.best_move.has_value()) {
                        // Retain the previous principal move explicitly
                        // even when the TT entry was displaced by a
                        // shallow verification or a hash resize.
                        context.ordering.order(root, legal_moves,
                                               result.best_move, 0);
                    }
                }

                bool root_authoritative = false;
                int score = context.negamax(
                    root, depth, alpha, beta, 0, pv, std::nullopt, true,
                    kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                    nullptr, &root_authoritative);
                if (!context.aborted && previous_score.has_value()) {
                    // Widen the failed side progressively. This preserves
                    // the useful narrow window when the iteration is
                    // stable, while avoiding repeated full-width searches
                    // after a tactical score jump.
                    for (int retry = 0; retry < 3 &&
                         (score <= alpha || score >= beta) && !context.aborted; ++retry) {
                        ++context.stats.aspiration_researches;
                        aspiration_researched = true;
                        aspiration_window = std::min(kInfinity / 4,
                                                    aspiration_window * 2 + 16);
                        if (score <= alpha) {
                            alpha = std::max(-kInfinity, alpha - aspiration_window);
                        }
                        if (score >= beta) {
                            beta = std::min(kInfinity, beta + aspiration_window);
                        }
                        pv = {};
                        score = context.negamax(
                            root, depth, alpha, beta, 0, pv, std::nullopt, true,
                            kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                            nullptr, &root_authoritative);
                    }
                    if (!context.aborted && (score <= alpha || score >= beta)) {
                        ++context.stats.aspiration_researches;
                        aspiration_researched = true;
                        pv = {};
                        score = context.negamax(
                            root, depth, -kInfinity, kInfinity, 0, pv, std::nullopt, true,
                            kMaximumCheckExtensionsPerPath, Move::no_move(), nullptr,
                            nullptr, &root_authoritative);
                    }
                }
                if (context.aborted) {
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                &context.stats, allow_deep_checked_fallback, &fallback_control,
                                true, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        ultra_short_checked_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                &context.stats, false, &fallback_control, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    if (result.completed_depth == 0 && !used_short_fallback) {
                        // An interrupted first iteration is never an
                        // authoritative tactical result.  Reject a
                        // partial root move that exposes an immediate
                        // checking reply or broad checking horizon even
                        // when the root move list did not contain a
                        // capture or direct check.
                        const auto partial = context.best_completed_root_move(root, true);
                        const auto partial_metadata = partial.has_value() ?
                            std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&partial](const MoveMetadata& metadata) {
                                    return metadata.move == partial->move;
                                }) : legal_moves.end();
                        if (partial.has_value() && root.is_legal(partial->move) &&
                            partial_metadata != legal_moves.end() &&
                            !root_move_is_broad_quiet_check(root, *partial_metadata)) {
                            result.best_move = partial->move;
                            result.pv = {partial->move};
                            result.score_cp = partial->score;
                        } else if (const auto fallback = first_safe_short_search_move(
                                       root, legal_moves);
                                   fallback.has_value()) {
                            result.best_move = *fallback;
                            result.pv = {*fallback};
                        }
                    }
                    break;
                }

                // Publication requires a complete root-score coverage plus a
                // valid PV. Strict provenance is tracked separately through
                // `root_authoritative` (the negamax root flag) and only gates
                // aspiration seeding and proof-sensitive metadata; ordinary
                // selective qsearch paths must not suppress UCI depth
                // progress or replace a completed iteration with an
                // unsearched ordered fallback.
                const bool root_result_publishable = context.root_result_complete(pv);
                if (!root_result_publishable) {
                    // The recursive search did not return a complete root
                    // result (missing score coverage or PV). Preserve the
                    // last completed result and use this work only to pace
                    // the next attempt.
                    const auto elapsed = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started);
                    previous_iteration_elapsed = std::max(
                        std::chrono::milliseconds{1}, elapsed - previous_report_elapsed);
                    if ((!unbounded && depth == maximum_depth) || root_is_forced_draw ||
                        time_manager.should_stop_after_iteration()) {
                        break;
                    }
                    continue;
                }

                result.completed_depth = depth;
                result.score_cp = score;
                result.mate = mate_from_score(score);
                if (pv.length > 0) {
                    result.best_move = pv.moves[0];
                    result.pv = pv.to_vector();
                }
                // The bounded short-search correction below is allowed to
                // improve the move handed to the caller, but its static
                // score is not an iterative-deepening proof.  Retain the
                // ordinary full-width result for aspiration and timing
                // observations so a heuristic safety choice cannot make
                // the next iteration look artificially unstable.
                const int searched_iteration_score = score;
                const bool searched_best_move_changed = had_completed_iteration &&
                    previous_best_move != result.best_move;
                const bool searched_pv_changed = had_completed_iteration &&
                    previous_pv != result.pv;
                bool bounded_fallback_result = false;

                if ((short_tactical_fallback || defer_nonchecking_short_fallback) &&
                    !defer_expensive_short_fallback &&
                    result.completed_depth == 1 &&
                    (short_capture_needs_fallback(legal_moves, result.best_move) ||
                     short_quiet_needs_fallback(root, legal_moves, result.best_move) ||
                     short_quiet_exposes_immediate_check(
                         root, legal_moves, result.best_move) ||
                     short_check_needs_fallback(legal_moves, result.best_move) ||
                     short_checked_root_king_evasion_needs_fallback(
                         root, legal_moves, result.best_move))) {
                    // The iteration is complete, so preserve a bounded
                    // shallow safety correction even if the hard clock
                    // has just fired. Do not re-enter the recursive
                    // emergency scan after the deadline.
                    auto fallback = short_search_fallback_move(
                        root, legal_moves, *evaluator, evaluator_mutex_ptr,
                        &context.stats, allow_deep_checked_fallback,
                        &fallback_control, true, true);
                    std::optional<Move> fallback_move;
                    int fallback_score = score;
                    if (fallback.has_value()) {
                        fallback_move = fallback->move;
                        fallback_score = normalize_root_fallback_score(fallback->score);
                    }
                    const auto current_metadata = result.best_move.has_value() ?
                        std::find_if(
                            legal_moves.begin(), legal_moves.end(),
                            [&result](const MoveMetadata& metadata) {
                                return metadata.move == *result.best_move;
                            }) : legal_moves.end();
                    if (current_metadata != legal_moves.end() &&
                        !current_metadata->is_capture() && !current_metadata->gives_check &&
                        current_metadata->move.promotion() == Promotion::none &&
                        (!fallback_move.has_value() ||
                         *fallback_move == current_metadata->move)) {
                        if (const auto checking_capture = scored_safe_checking_capture(
                                root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                allow_deep_checked_fallback, &fallback_control);
                            checking_capture.has_value()) {
                            fallback_move = checking_capture->move;
                            fallback_score = normalize_root_fallback_score(checking_capture->score);
                        } else {
                            fallback_move.reset();
                        }
                    }
                    if (fallback_move.has_value() &&
                        short_fallback_should_replace_completed_root(
                            root, legal_moves, result.best_move, *fallback_move)) {
                        result.best_move = *fallback_move;
                        result.pv = {*fallback_move};
                        result.score_cp = fallback_score;
                        result.mate = mate_from_score(fallback_score);
                        score = fallback_score;
                        bounded_fallback_result = true;
                        // Keep the protocol PV in lockstep with the
                        // corrected authoritative root move. The normal
                        // iteration PV still contains the pre-correction
                        // move, and publishing it here would make the
                        // following bestmove disagree with the final
                        // info line.
                        pv = {};
                        pv.moves[0] = *fallback_move;
                        pv.length = 1;
                    }
                }

                // Selective root checks below may run another negamax call,
                // which refreshes the context's root-score scratch buffer.
                // Preserve the completed full-width scores before any such
                // check so later safety decisions remain based on this
                // iteration's complete root ordering.
                std::vector<SearchContext::RootMoveScore> root_score_snapshot;
                if (limits.depth.has_value() &&
                    (depth == kRootShallowConfirmationDepth ||
                     depth == kRootSelectiveDepth) &&
                    context.root_move_score_count != 0) {
                    root_score_snapshot.assign(
                        context.root_move_scores.begin(),
                        context.root_move_scores.begin() + context.root_move_score_count);
                }
                // Some bounded root safety passes compare candidates at a
                // deeper horizon to expose a shallow tactical trap.  The
                // selected move still belongs to the requested iteration,
                // so refresh its line at the nominal depth before
                // publishing the score/PV.  This keeps fixed-depth UCI
                // results and timing observations tied to one horizon.
                const auto nominal_root_verification =
                    [&context, &root, &legal_moves](const Move move, const int nominal_depth)
                    -> std::optional<SearchContext::RootVerification> {
                    const auto metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [move](const MoveMetadata& candidate) {
                            return candidate.move == move;
                        });
                    if (metadata == legal_moves.end()) {
                        return std::nullopt;
                    }
                    ++context.stats.root_selective_candidates;
                    const SearchContext::RootVerification verification =
                        context.selectively_research_root_move(root, *metadata, nominal_depth);
                    if (context.aborted || !verification.authoritative) {
                        return std::nullopt;
                    }
                    return verification;
                };

                if (limits.depth.has_value() &&
                    *limits.depth == kRootShallowConfirmationDepth &&
                    depth == kRootShallowConfirmationDepth &&
                    !limits.search_moves_specified && !context.aborted &&
                    !root_score_snapshot.empty() && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    struct ShallowRootCandidate {
                        MoveMetadata metadata{};
                        int score = -kInfinity;
                        bool forcing = false;
                    };

                    const auto metadata_for = [&legal_moves](const Move move) {
                        return std::find_if(
                            legal_moves.begin(), legal_moves.end(),
                            [move](const MoveMetadata& metadata) {
                                return metadata.move == move;
                            });
                    };
                    const auto incumbent_metadata = metadata_for(*result.best_move);
                    const bool incumbent_forcing = incumbent_metadata != legal_moves.end() &&
                        (incumbent_metadata->is_capture() || incumbent_metadata->gives_check ||
                         incumbent_metadata->move.promotion() != Promotion::none);
                    const auto is_same_target_capture =
                        [&incumbent_metadata, &legal_moves](
                            const auto metadata) {
                        return incumbent_metadata != legal_moves.end() &&
                            metadata != legal_moves.end() &&
                            incumbent_metadata->is_capture() && metadata->is_capture() &&
                            metadata->move.to() == incumbent_metadata->move.to();
                    };
                    const bool has_mixed_forcing_alternative = incumbent_metadata != legal_moves.end() &&
                        std::any_of(
                            root_score_snapshot.begin(), root_score_snapshot.end(),
                            [&metadata_for, &legal_moves, &incumbent_forcing,
                             &is_same_target_capture, &result](
                                const SearchContext::RootMoveScore& root_score) {
                                if (root_score.move == *result.best_move) {
                                    return false;
                                }
                                const auto metadata = metadata_for(root_score.move);
                                if (metadata == legal_moves.end()) {
                                    return false;
                                }
                                const bool forcing = metadata->is_capture() || metadata->gives_check ||
                                    metadata->move.promotion() != Promotion::none;
                                return (root_score.exact && forcing != incumbent_forcing) ||
                                    is_same_target_capture(metadata);
                            });
                    if (incumbent_metadata != legal_moves.end() &&
                        has_mixed_forcing_alternative) {
                        std::vector<ShallowRootCandidate> alternatives;
                        alternatives.reserve(root_score_snapshot.size());
                        for (const SearchContext::RootMoveScore& root_score :
                             root_score_snapshot) {
                            if (root_score.move == *result.best_move) {
                                continue;
                            }
                            const auto metadata = metadata_for(root_score.move);
                            if (metadata == legal_moves.end()) {
                                continue;
                            }
                            const bool same_target_capture =
                                is_same_target_capture(metadata);
                            if (!root_score.exact && !same_target_capture) {
                                // A scout bound may nominate a defensive
                                // recapture cluster, but no other
                                // inexact root line may enter the
                                // authoritative confirmation set.
                                continue;
                            }
                            const bool forcing = metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none;
                            const int score_margin = forcing ? kRootShallowForcingMargin :
                                kRootShallowConfirmationMargin;
                            if (root_score.score < score - score_margin) {
                                continue;
                            }
                            alternatives.push_back(
                                ShallowRootCandidate{*metadata, root_score.score, forcing});
                        }
                        std::stable_sort(
                            alternatives.begin(), alternatives.end(),
                            [](const ShallowRootCandidate& left,
                               const ShallowRootCandidate& right) {
                                if (left.forcing != right.forcing) {
                                    return left.forcing;
                                }
                                if (left.score != right.score) {
                                    return left.score > right.score;
                                }
                                return detail::move_tie_break_key(left.metadata.move) <
                                    detail::move_tie_break_key(right.metadata.move);
                            });

                        std::array<ShallowRootCandidate,
                                   kMaximumRootShallowConfirmationCandidates> candidates{};
                        std::size_t candidate_count = 0;
                        candidates[candidate_count++] =
                            ShallowRootCandidate{*incumbent_metadata, score,
                                                  incumbent_metadata->is_capture() ||
                                                      incumbent_metadata->gives_check ||
                                                      incumbent_metadata->move.promotion() !=
                                                          Promotion::none};
                        for (const ShallowRootCandidate& candidate : alternatives) {
                            if (candidate_count == candidates.size()) {
                                break;
                            }
                            candidates[candidate_count++] = candidate;
                        }

                        int confirmed_score = -kInfinity;
                        std::optional<Move> confirmed_move;
                        PrincipalVariation confirmed_pv;
                        for (std::size_t index = 0; index < candidate_count; ++index) {
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                context.selectively_research_root_move(
                                    root, candidates[index].metadata,
                                    kRootShallowConfirmationDepth);
                            if (context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (!confirmed_move.has_value() ||
                                verification.score > confirmed_score) {
                                confirmed_score = verification.score;
                                confirmed_move = candidates[index].metadata.move;
                                confirmed_pv = verification.pv;
                            }
                        }
                        if (!context.aborted && confirmed_move.has_value()) {
                            if (confirmed_move != result.best_move ||
                                confirmed_score != score) {
                                ++context.stats.root_selective_researches;
                            }
                            score = confirmed_score;
                            result.score_cp = confirmed_score;
                            result.mate = mate_from_score(confirmed_score);
                            result.best_move = confirmed_move;
                            pv = confirmed_pv;
                            result.pv = confirmed_pv.length > 0 ? confirmed_pv.to_vector() :
                                std::vector<Move>{*confirmed_move};
                            bounded_fallback_result = false;
                        }
                    }
                }

                // A forcing capture can look marginally best at the first
                // full-width iteration while a quiet escape survives one
                // more ply.  Re-search a small capture-versus-quiet band
                // only for explicit depth-one diagnostics; timed searches
                // and deeper iterations retain their normal budget.
                const bool shallow_root_research =
                    (limits.depth.has_value() ||
                     (limits.movetime.has_value() &&
                      *limits.movetime <= kShortTimedSerialThreshold)) &&
                    !limits.search_moves_specified;
                const bool short_timed_root_research =
                    limits.movetime.has_value() &&
                    *limits.movetime >= kShortTimedFallbackMinimum &&
                    *limits.movetime <= kShortTimedSerialThreshold;
                const int shallow_root_margin = short_timed_root_research ? 50 : kRootSelectiveMargin;
                if (shallow_root_research &&
                    depth == 1 && !context.aborted && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() && best_metadata->is_capture()) {
                        std::array<MoveMetadata, 3> candidates{};
                        std::size_t candidate_count = 0;
                        struct QuietCandidate {
                            int score = -kInfinity;
                            MoveMetadata metadata{};
                        };
                        std::array<QuietCandidate, 2> quiet_candidates{};
                        std::size_t quiet_count = 0;
                        for (std::size_t index = 0; index < context.root_move_score_count; ++index) {
                            const SearchContext::RootMoveScore& root_score =
                                context.root_move_scores[index];
                            // This band only nominates a candidate for a
                            // deeper full-window comparison. A scout
                            // score is useful discovery evidence here; it
                            // never becomes authoritative by itself.
                            if (!short_timed_root_research &&
                                root_score.score < score - shallow_root_margin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() || metadata->move == *result.best_move ||
                                metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            QuietCandidate quiet{root_score.score, *metadata};
                            if (quiet_count < quiet_candidates.size()) {
                                quiet_candidates[quiet_count++] = quiet;
                            } else if (root_score.score > quiet_candidates.back().score) {
                                quiet_candidates.back() = quiet;
                            } else {
                                continue;
                            }
                            for (std::size_t quiet_index = quiet_count; quiet_index > 1; --quiet_index) {
                                if (quiet_candidates[quiet_index - 2].score >=
                                    quiet_candidates[quiet_index - 1].score) {
                                    break;
                                }
                                std::swap(quiet_candidates[quiet_index - 2],
                                          quiet_candidates[quiet_index - 1]);
                            }
                        }

                        candidates[candidate_count++] = *best_metadata;
                        for (std::size_t quiet_index = 0;
                             quiet_index < quiet_count && candidate_count < candidates.size();
                             ++quiet_index) {
                            candidates[candidate_count++] = quiet_candidates[quiet_index].metadata;
                        }
                        const bool has_quiet_candidate = quiet_count != 0;
                        if (has_quiet_candidate) {
                            int extended_score = -kInfinity;
                            std::optional<Move> extended_move;
                            PrincipalVariation extended_pv;
                            for (std::size_t index = 0; index < candidate_count; ++index) {
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(root, candidates[index],
                                                                           depth + 1);
                                if (context.aborted) {
                                    break;
                                }
                                if (!verification.authoritative) {
                                    continue;
                                }
                                if (!extended_move.has_value() || verification.score > extended_score) {
                                    extended_score = verification.score;
                                    extended_move = candidates[index].move;
                                    extended_pv = verification.pv;
                                }
                            }
                            // Compare the candidates at the same deeper horizon.  A
                            // shallow capture can carry a horizon bonus, so comparing a
                            // deeper quiet score against the old shallow score would keep
                            // poisoned captures.  A later candidate may hit the hard
                            // deadline; in that case the best completed candidate remains
                            // authoritative.
                            bool nominal_score_authoritative = false;
                            if (extended_move.has_value() && !context.aborted) {
                                if (const auto nominal = nominal_root_verification(
                                        *extended_move, depth); nominal.has_value()) {
                                    extended_score = nominal->score;
                                    extended_pv = nominal->pv;
                                    nominal_score_authoritative = true;
                                }
                            }
                            if (extended_move.has_value() && nominal_score_authoritative &&
                                !context.aborted) {
                                if (extended_move != result.best_move || extended_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = extended_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                 result.best_move = extended_move;
                                 pv = extended_pv;
                                 result.pv = extended_pv.to_vector();
                                 bounded_fallback_result = false;
                             }
                        }
                    } else if (best_metadata != legal_moves.end() &&
                               !best_metadata->is_capture() && !best_metadata->gives_check &&
                               best_metadata->move.promotion() == Promotion::none) {
                        // The inverse horizon trap is a quiet move that is only a few
                        // centipawns ahead of a check, capture, or promotion.  At an
                        // incomplete depth-one root the quiet score is not authoritative;
                        // compare the quiet move and a small forcing band at depth two.
                        // A quiet move can itself be forcing (for example, a fork or
                        // direct attack) even though it has no check flag. Include those
                        // moves in the same confirmation band so root ordering does not
                        // discard the tactical signal already used by the recursive search.
                        const PositionFeatures root_features = root.position_features();
                        const auto is_root_forcing_candidate = [&root, &root_features](
                            const MoveMetadata& metadata) {
                            if (metadata.is_capture() || metadata.gives_check ||
                                metadata.move.promotion() != Promotion::none) {
                                return true;
                            }
                            if (!quiet_move_has_direct_forcing_target(root_features, metadata) ||
                                !root.make_search_move(metadata)) {
                                return false;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            return forcing;
                        };
                        std::array<MoveMetadata, 4> candidates{};
                        std::size_t candidate_count = 0;
                        bool has_quiet_forcing_candidate = false;
                        candidates[candidate_count++] = *best_metadata;
                        const auto append_candidates = [&] (const bool quiet_only) {
                            for (std::size_t index = 0;
                                 index < context.root_move_score_count &&
                                 candidate_count < candidates.size(); ++index) {
                                const SearchContext::RootMoveScore& root_score =
                                    context.root_move_scores[index];
                                // Scout scores may nominate a candidate;
                                // the bounded re-search below supplies the
                                // only score used for replacement.
                                if (root_score.move == *result.best_move ||
                                    root_score.score < score - shallow_root_margin) {
                                    continue;
                                }
                                const auto metadata = std::find_if(
                                    legal_moves.begin(), legal_moves.end(),
                                    [&root_score](const MoveMetadata& move_metadata) {
                                        return move_metadata.move == root_score.move;
                                    });
                                if (metadata == legal_moves.end()) {
                                    continue;
                                }
                                const bool conventional_forcing = metadata->is_capture() ||
                                    metadata->gives_check ||
                                    metadata->move.promotion() != Promotion::none;
                                if (quiet_only) {
                                    if (conventional_forcing ||
                                        !is_root_forcing_candidate(*metadata)) {
                                        continue;
                                    }
                                    has_quiet_forcing_candidate = true;
                                } else if (!conventional_forcing) {
                                    continue;
                                }
                                candidates[candidate_count++] = *metadata;
                            }
                        };
                        // Keep checks/captures/promotions ahead of the more
                        // expensive quiet-forcing confirmation candidates.
                        append_candidates(false);
                        append_candidates(true);

                        if (candidate_count > 1) {
                            int extended_score = -kInfinity;
                            std::optional<Move> extended_move;
                            bool extended_forcing = false;
                            PrincipalVariation extended_pv;
                            const int confirmation_depth =
                                has_quiet_forcing_candidate && limits.depth.has_value() ?
                                    depth + 2 : depth + 1;
                            for (std::size_t index = 0; index < candidate_count; ++index) {
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(root, candidates[index],
                                                                           confirmation_depth);
                                if (context.aborted) {
                                    break;
                                }
                                if (!verification.authoritative) {
                                    continue;
                                }
                                const bool candidate_forcing = candidates[index].is_capture() ||
                                    candidates[index].gives_check ||
                                    candidates[index].move.promotion() != Promotion::none;
                                if (!extended_move.has_value() || verification.score > extended_score ||
                                    (verification.score == extended_score && candidate_forcing &&
                                     !extended_forcing)) {
                                    extended_score = verification.score;
                                    extended_move = candidates[index].move;
                                    extended_forcing = candidate_forcing;
                                    extended_pv = verification.pv;
                                }
                            }
                            bool nominal_score_authoritative = false;
                            if (extended_move.has_value() && !context.aborted) {
                                if (const auto nominal = nominal_root_verification(
                                        *extended_move, depth); nominal.has_value()) {
                                    extended_score = nominal->score;
                                    extended_pv = nominal->pv;
                                    nominal_score_authoritative = true;
                                }
                            }
                            if (extended_move.has_value() && nominal_score_authoritative &&
                                !context.aborted) {
                                if (extended_move != result.best_move || extended_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = extended_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = extended_move;
                                pv = extended_pv;
                                result.pv = extended_pv.to_vector();
                            }
                        }
                    }
                }

                // A shallow fixed-depth iteration can leave two quiet root moves
                // inside the same narrow score band even though one move's first
                // forcing consequence is one ply deeper. Re-search only the first
                // ordered alternative in that band, and only at the diagnostic
                // depth where this horizon is meaningful. Timed searches and
                // deeper fixed-depth searches retain the normal PVS path.
                if (limits.depth.has_value() && !limits.search_moves_specified &&
                    depth == kRootSelectiveDepth &&
                    !context.aborted && result.best_move.has_value() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && !best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none) {
                        std::array<MoveMetadata, 3> candidates{};
                        std::size_t candidate_count = 0;
                        std::vector<MoveMetadata> forcing_candidates;
                        std::vector<MoveMetadata> quiet_candidates;
                        const PositionFeatures root_features = root.position_features();
                        const auto is_forcing_quiet = [&root, &root_features](
                            const MoveMetadata& metadata) {
                            if (!root.make_search_move(metadata)) {
                                return false;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            return forcing;
                        };
                        for (std::size_t index = 0; index < context.root_move_score_count; ++index) {
                            const SearchContext::RootMoveScore& root_score =
                                context.root_move_scores[index];
                            // A scout bound is discovery-only. Every
                            // candidate selected from this band is
                            // re-searched before it can change the root.
                            if (root_score.move == *result.best_move ||
                                root_score.score < score - kRootSelectiveMargin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata != legal_moves.end() && !metadata->is_capture() &&
                                !metadata->gives_check &&
                                metadata->move.promotion() == Promotion::none) {
                                (is_forcing_quiet(*metadata) ? forcing_candidates :
                                    quiet_candidates).push_back(*metadata);
                            }
                        }

                        const auto append_candidates = [&candidates, &candidate_count](
                            const std::vector<MoveMetadata>& source) {
                            for (const MoveMetadata& metadata : source) {
                                if (candidate_count == candidates.size()) {
                                    break;
                                }
                                candidates[candidate_count++] = metadata;
                            }
                        };
                        append_candidates(forcing_candidates);
                        append_candidates(quiet_candidates);

                        const int shallow_score = score;
                        for (std::size_t index = 0; index < candidate_count; ++index) {
                            const MoveMetadata& candidate = candidates[index];
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                context.selectively_research_root_move(root, candidate,
                                                                       depth + 1);
                            if (context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (verification.score > shallow_score + kRootSelectiveImprovement) {
                                const auto nominal = nominal_root_verification(
                                    candidate.move, depth);
                                if (!nominal.has_value()) {
                                    continue;
                                }
                                ++context.stats.root_selective_researches;
                                score = nominal->score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = candidate.move;
                                pv = nominal->pv;
                                result.pv = pv.to_vector();
                                bounded_fallback_result = false;
                                break;
                            }
                        }
                    }
                }

                // A shallow root can also leave two quiet checking moves
                // with nearly identical scores.  The incumbent may be an
                // exact first-move result while the alternative is only a
                // PVS upper bound, so ordinary root ranking is not a
                // comparable check-versus-check decision.  Re-search the
                // incumbent and at most two near-tied quiet checks at one
                // common full-window horizon; this is restricted to the
                // explicit depth-three diagnostic path and does not alter
                // timed or deeper searches.
                if (limits.depth.has_value() && !limits.search_moves_specified &&
                    depth == kRootSelectiveDepth &&
                    !context.aborted && result.best_move.has_value() &&
                    !root_score_snapshot.empty() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none) {
                        std::vector<std::pair<int, MoveMetadata>> alternatives;
                        for (const SearchContext::RootMoveScore& root_score :
                             root_score_snapshot) {
                            if (root_score.move == *result.best_move ||
                                root_score.score < score - kRootSelectiveMargin) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() || metadata->is_capture() ||
                                !metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            alternatives.emplace_back(root_score.score, *metadata);
                        }
                        std::stable_sort(
                            alternatives.begin(), alternatives.end(),
                            [](const auto& left, const auto& right) {
                                if (left.first != right.first) {
                                    return left.first > right.first;
                                }
                                return detail::move_tie_break_key(left.second.move) <
                                    detail::move_tie_break_key(right.second.move);
                            });
                        if (alternatives.size() > kMaximumRootCheckConfirmationCandidates) {
                            alternatives.resize(kMaximumRootCheckConfirmationCandidates);
                        }

                        if (!alternatives.empty()) {
                            int confirmed_score = -kInfinity;
                            std::optional<Move> confirmed_move;
                            PrincipalVariation confirmed_pv;
                            ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification incumbent_verification =
                                    context.selectively_research_root_move(
                                        root, *best_metadata, depth + 1);
                            if (!context.aborted && incumbent_verification.authoritative) {
                                confirmed_score = incumbent_verification.score;
                                confirmed_move = best_metadata->move;
                                confirmed_pv = incumbent_verification.pv;
                            }
                            for (const auto& [unused_shallow_score, candidate] : alternatives) {
                                (void)unused_shallow_score;
                                if (context.aborted) {
                                    break;
                                }
                                ++context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    context.selectively_research_root_move(
                                        root, candidate, depth + 1);
                                if (context.aborted) {
                                    break;
                                }
                                if (!verification.authoritative) {
                                    continue;
                                }
                                if (!confirmed_move.has_value() ||
                                    verification.score > confirmed_score) {
                                    confirmed_score = verification.score;
                                    confirmed_move = candidate.move;
                                    confirmed_pv = verification.pv;
                                }
                            }
                            if (!context.aborted && confirmed_move.has_value()) {
                                // The deeper confirmation chooses the move,
                                // but the public result still represents
                                // the requested fixed depth.  Refresh the
                                // selected line at that nominal depth so a
                                // deeper horizon score is never published
                                // as the depth-three evaluation.
                                const auto confirmed_metadata = std::find_if(
                                    legal_moves.begin(), legal_moves.end(),
                                    [&confirmed_move](const MoveMetadata& metadata) {
                                        return metadata.move == *confirmed_move;
                                    });
                                bool nominal_score_authoritative = false;
                                if (confirmed_metadata != legal_moves.end()) {
                                    ++context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification nominal_verification =
                                        context.selectively_research_root_move(
                                            root, *confirmed_metadata, depth);
                                    if (!context.aborted && nominal_verification.authoritative) {
                                        confirmed_score = nominal_verification.score;
                                        confirmed_pv = nominal_verification.pv;
                                        nominal_score_authoritative = true;
                                    }
                                }
                                if (!nominal_score_authoritative) {
                                    confirmed_move.reset();
                                }
                            }
                            if (!context.aborted && confirmed_move.has_value()) {
                                if (confirmed_move != result.best_move ||
                                    confirmed_score != score) {
                                    ++context.stats.root_selective_researches;
                                }
                                score = confirmed_score;
                                result.score_cp = score;
                                result.mate = mate_from_score(score);
                                result.best_move = confirmed_move;
                                pv = confirmed_pv;
                                result.pv = confirmed_pv.length > 0 ? confirmed_pv.to_vector() :
                                    std::vector<Move>{*confirmed_move};
                                bounded_fallback_result = false;
                            }
                        }
                    }
                }

                // A quiet king move that removes active king-zone pressure
                // deserves a bounded confirmation when a shallow fixed-depth
                // root prefers a non-forcing move.  Compare the current root
                // move and at most two ordered king escapes at depth five;
                // this is intentionally limited to an explicit depth-three
                // diagnostic search so normal clock searches retain their
                // ordinary iterative-deepening budget.
                if (limits.depth.has_value() && *limits.depth == kRootSelectiveDepth &&
                    depth == kRootSelectiveDepth && !limits.search_moves_specified &&
                    !context.aborted && result.best_move.has_value() &&
                    !root_score_snapshot.empty() && !root.in_check() &&
                    !mate_from_score(score).has_value()) {
                    const auto best_metadata = std::find_if(
                        legal_moves.begin(), legal_moves.end(),
                        [&result](const MoveMetadata& metadata) {
                            return metadata.move == *result.best_move;
                        });
                    const PositionFeatures root_features = root.position_features();
                    const std::size_t own_side = root.side_to_move() == Color::white ? 0U : 1U;
                    if (best_metadata != legal_moves.end() &&
                        !best_metadata->is_capture() && !best_metadata->gives_check &&
                        best_metadata->move.promotion() == Promotion::none &&
                        root_features.game_phase >= 8 && root_features.fullmove_number > 10 &&
                        root_features.king_zone_attacks[own_side] != 0) {
                        std::vector<std::pair<int, MoveMetadata>> defensive_candidates;
                        for (const SearchContext::RootMoveScore& root_score : root_score_snapshot) {
                            // Use the shallow value only to prioritize a
                            // bounded candidate list; depth-five
                            // confirmation remains authoritative.
                            if (root_score.move == *result.best_move) {
                                continue;
                            }
                            const auto metadata = std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&root_score](const MoveMetadata& move_metadata) {
                                    return move_metadata.move == root_score.move;
                                });
                            if (metadata == legal_moves.end() ||
                                metadata->moving_piece != PieceType::king ||
                                metadata->is_capture() || metadata->gives_check ||
                                metadata->move.promotion() != Promotion::none) {
                                continue;
                            }
                            if (!root.make_search_move(*metadata)) {
                                continue;
                            }
                            const PositionFeatures after_features = root.position_features();
                            (void)root.unmake_move();
                            if (after_features.king_zone_attacks[own_side] >=
                                root_features.king_zone_attacks[own_side]) {
                                continue;
                            }
                            defensive_candidates.emplace_back(root_score.score, *metadata);
                        }
                        std::stable_sort(
                            defensive_candidates.begin(), defensive_candidates.end(),
                            [](const auto& left, const auto& right) {
                                return left.first > right.first;
                            });
                        if (defensive_candidates.size() > kMaximumRootKingSafetyCandidates) {
                            defensive_candidates.resize(kMaximumRootKingSafetyCandidates);
                        }

                        if (!defensive_candidates.empty()) {
                            ++context.stats.king_safety_extensions;
                            ++context.stats.root_selective_candidates;
                            const SearchContext::RootVerification best_verification =
                                context.selectively_research_root_move(
                                    root, *best_metadata, kRootKingSafetySelectiveDepth);
                            if (!context.aborted && best_verification.authoritative) {
                                int selected_score = best_verification.score;
                                Move selected_move = best_metadata->move;
                                PrincipalVariation selected_pv = best_verification.pv;
                                for (const auto& [unused_shallow_score, candidate] :
                                     defensive_candidates) {
                                    (void)unused_shallow_score;
                                    ++context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification verification =
                                        context.selectively_research_root_move(
                                            root, candidate, kRootKingSafetySelectiveDepth);
                                    if (context.aborted) {
                                        break;
                                    }
                                    if (!verification.authoritative) {
                                        continue;
                                    }
                                    if (verification.score > selected_score ||
                                        verification.score >= best_verification.score -
                                            kRootKingSafetyTieMargin) {
                                        selected_score = verification.score;
                                        selected_move = candidate.move;
                                        selected_pv = verification.pv;
                                    }
                                }
                                bool nominal_score_authoritative = false;
                                if (!context.aborted) {
                                    if (const auto nominal = nominal_root_verification(
                                            selected_move, depth); nominal.has_value()) {
                                        selected_score = nominal->score;
                                        selected_pv = nominal->pv;
                                        nominal_score_authoritative = true;
                                    }
                                }
                                if (nominal_score_authoritative && !context.aborted) {
                                    if (selected_move != result.best_move ||
                                        selected_score != result.score_cp) {
                                        ++context.stats.root_selective_researches;
                                    }
                                    result.score_cp = selected_score;
                                    result.mate = mate_from_score(selected_score);
                                     result.best_move = selected_move;
                                     pv = selected_pv;
                                     result.pv = pv.length > 0 ? pv.to_vector() :
                                         std::vector<Move>{selected_move};
                                     score = selected_score;
                                     bounded_fallback_result = false;
                                 }
                            }
                        }
                    }
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                const std::uint64_t visited = context.stats.nodes + context.stats.qnodes;
                SearchInfo info{depth, score, result.mate, visited,
                                elapsed.count() > 0 ? visited * 1000 /
                                    static_cast<std::uint64_t>(elapsed.count()) : visited,
                                elapsed, pv.to_vector()};
                info.seldepth = context.stats.seldepth;
                info.qnodes = context.stats.qnodes;
                info.tt_hits = context.stats.tt_hits;
                info.tbhits = context.stats.tbhits;
                safely_report_info(sink, info);
                const int iteration_observation_score = bounded_fallback_result ?
                    searched_iteration_score : score;
                time_manager.observe_iteration(SearchIterationObservation{
                    depth,
                    iteration_observation_score,
                    bounded_fallback_result ? searched_best_move_changed :
                        (had_completed_iteration && previous_best_move != result.best_move),
                    bounded_fallback_result ? searched_pv_changed :
                        (had_completed_iteration && previous_pv != result.pv),
                    aspiration_researched,
                    visited});
                result.timing = time_manager.diagnostics();
                // Keep a bounded safety score visible in the result, but
                // anchor the next aspiration window to the full-width
                // search score when that correction is still active.
                if (root_authoritative) {
                    previous_score = iteration_observation_score;
                } else {
                    // A complete selective iteration is useful to the UCI
                    // caller, but its score is not a safe aspiration
                    // center. Force the next iteration to establish a
                    // fresh full-window baseline.
                    previous_score.reset();
                }
                previous_iteration_elapsed = elapsed - previous_report_elapsed;
                previous_report_elapsed = elapsed;

                if ((!unbounded && depth == maximum_depth) ||
                    time_manager.should_stop_after_iteration()) {
                    break;
                }
            }
            result.stats = context.stats;
        } else {
            std::atomic<std::uint64_t> global_nodes = 0;
            std::atomic<std::uint64_t>* global_nodes_ptr = time_manager.node_limit().has_value() ?
                &global_nodes : nullptr;
            const std::size_t worker_count = std::min(options.threads, legal_moves.size());
            const bool multi_pv = options.multi_pv > 1;
            RootWorkerPool pool(worker_count, *evaluator, *table, time_manager,
                                session->stop_requested(), global_nodes_ptr, evaluator_mutex_ptr, true,
                                options.quiet_history_side_hook, tablebase_binding);
            SearchStats total_stats;
            auto root_context_storage = std::make_unique<SearchContext>(
                *evaluator, *table, time_manager, session->stop_requested(),
                global_nodes_ptr, evaluator_mutex_ptr, nullptr, true,
                options.quiet_history_side_hook, tablebase_binding);
            SearchContext& root_context = *root_context_storage;

            MoveMetadataList parallel_moves = legal_moves;
            detail::SearchMoveOrdering root_ordering;
            root_ordering.order(root, parallel_moves, std::nullopt, 0);
            std::vector<RootScheduleRecord> root_schedule;
            root_schedule.reserve(parallel_moves.size());
            for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                root_schedule.push_back(
                    RootScheduleRecord{
                        parallel_moves[index].move, index, -kInfinity, false, false});
            }
            const auto schedule_record_for = [&root_schedule](const Move move)
                -> RootScheduleRecord* {
                const auto record = std::find_if(
                    root_schedule.begin(), root_schedule.end(),
                    [move](const RootScheduleRecord& candidate) {
                        return candidate.move == move;
                    });
                return record == root_schedule.end() ? nullptr : &*record;
            };
            bool used_short_fallback = false;
            const bool defer_nonchecking_short_fallback =
                ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root;
            if (short_tactical_fallback && !defer_nonchecking_short_fallback &&
                !defer_expensive_short_fallback) {
                if (const auto fallback = short_search_fallback_move(
                        root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                        &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                    fallback.has_value()) {
                    result.best_move = fallback->move;
                    result.pv = {fallback->move};
                    const int fallback_score = normalize_root_fallback_score(fallback->score);
                    result.score_cp = fallback_score;
                    result.mate = mate_from_score(fallback_score);
                    used_short_fallback = true;
                }
            } else if (short_search_fallback && !defer_nonchecking_short_fallback) {
                if (const auto fallback = first_safe_short_search_move(root, parallel_moves);
                    fallback.has_value()) {
                    result.best_move = *fallback;
                    result.pv = {*fallback};
                    used_short_fallback = true;
                }
            }
            int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            bool unbounded = limits.infinite || limits.ponder;
            if (limits.ponder && limits.depth.has_value()) {
                // Mirror of the serial driver: a ponder depth limit is a target,
                // not an iteration cap, so the search keeps deepening until
                // stop/ponderhit instead of repeating the final depth.
                maximum_depth = kMaximumSearchDepth;
            }
            std::optional<int> previous_score;
            std::chrono::milliseconds previous_iteration_elapsed{1};
            std::chrono::milliseconds previous_report_elapsed{0};
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                if (auto conversion = session->take_ponderhit_limits(); conversion.has_value()) {
                    limits = std::move(*conversion);
                    time_manager.reconfigure(limits, root.side_to_move(), options.speed_percent,
                                             options.move_overhead_ms, options.slow_mover_percent,
                                             timing_context);
                    maximum_depth = std::min(
                        kMaximumSearchDepth,
                        std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                    unbounded = limits.infinite || limits.ponder;
                }
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                if (!unbounded && !time_manager.should_start_next_iteration(previous_iteration_elapsed)) {
                    break;
                }
                if (session->stop_requested().load(std::memory_order_relaxed) ||
                    time_manager.should_stop(root_context.visited_nodes())) {
                    break;
                }

                root_context.begin_iteration(nullptr);
                if (!root_context.count_node(false)) {
                    accumulate_stats(total_stats, root_context.stats);
                    break;
                }
                const std::optional<std::chrono::milliseconds> iteration_time_budget =
                    time_manager.time_budget();
                const bool short_timed_root = iteration_time_budget.has_value() &&
                    *iteration_time_budget < kShortTimedFallbackMinimum;
                const bool root_check_extension = root.in_check() &&
                    depth < kMaximumSearchDepth && !short_timed_root;
                if (root_check_extension) {
                    ++root_context.stats.check_extensions;
                }

                std::optional<Move> tt_move;
                if (const auto entry = table_access.probe(search_transposition_key(root), 0);
                    entry.has_value()) {
                    ++root_context.stats.tt_hits;
                    if (!entry->best_move.is_no_move()) {
                        tt_move = entry->best_move;
                    }
                }
                // The previous completed root PV is the authoritative
                // iterative-deepening hint.  A TT move can come from a
                // displaced/partial worker and must not displace the
                // completed result at the root, matching Stockfish 19's
                // root PV precedence.
                if (result.completed_depth > 0 && result.best_move.has_value()) {
                    tt_move = result.best_move;
                }
                root_ordering.order(root, parallel_moves, tt_move, 0);
                if (depth > 1) {
                    std::vector<std::size_t> schedule_indices;
                    schedule_indices.reserve(parallel_moves.size());
                    for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                        schedule_indices.push_back(index);
                    }
                    std::stable_sort(
                        schedule_indices.begin(), schedule_indices.end(),
                        [&parallel_moves, &tt_move, &schedule_record_for](
                            const std::size_t left, const std::size_t right) {
                        const Move left_move = parallel_moves[left].move;
                        const Move right_move = parallel_moves[right].move;
                        const bool left_tt = tt_move.has_value() && left_move == *tt_move;
                        const bool right_tt = tt_move.has_value() && right_move == *tt_move;
                        if (left_tt != right_tt) {
                            return left_tt;
                        }

                        const RootScheduleRecord* left_record =
                            schedule_record_for(left_move);
                        const RootScheduleRecord* right_record =
                            schedule_record_for(right_move);
                        const bool left_scored = left_record != nullptr &&
                            left_record->has_previous_score;
                        const bool right_scored = right_record != nullptr &&
                            right_record->has_previous_score;
                        if (left_scored != right_scored) {
                            return left_scored;
                        }
                        const bool left_exact = left_scored && left_record->previous_exact;
                        const bool right_exact = right_scored && right_record->previous_exact;
                        if (left_exact != right_exact) {
                            return left_exact;
                        }
                        if (left_scored && right_scored &&
                            left_record->previous_score != right_record->previous_score) {
                            return left_record->previous_score > right_record->previous_score;
                        }
                        const std::size_t left_stable = left_record != nullptr ?
                            left_record->stable_index : left;
                        const std::size_t right_stable = right_record != nullptr ?
                            right_record->stable_index : right;
                        return left_stable < right_stable;
                    });

                    MoveMetadataList scheduled_moves;
                    for (const std::size_t index : schedule_indices) {
                        (void)scheduled_moves.push_back(parallel_moves[index]);
                    }
                    parallel_moves = scheduled_moves;
                }
                std::vector<std::size_t> stable_root_indices;
                stable_root_indices.reserve(parallel_moves.size());
                for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                    const RootScheduleRecord* record =
                        schedule_record_for(parallel_moves[index].move);
                    stable_root_indices.push_back(
                        record != nullptr ? record->stable_index : index);
                }

                std::vector<RootLine> lines(parallel_moves.size());

                SearchStats iteration_stats;
                bool aborted = false;
                bool aspiration_researched = false;
                int window_alpha = -kInfinity;
                int window_beta = kInfinity;
                int aspiration_window = kAspirationWindow + std::min(32, depth * 2);
                const bool use_aspiration = previous_score.has_value() && !multi_pv;
                if (use_aspiration) {
                    window_alpha = std::max(-kInfinity,
                                            *previous_score - aspiration_window);
                    window_beta = std::min(kInfinity,
                                           *previous_score + aspiration_window);
                }
                // Keep the two-worker fixed-depth path for non-checked
                // roots on shared root PVS so it benefits from the live
                // alpha bound.  At four workers,
                // fixed-depth benchmark parity is more important than the
                // racy order in which a moving root alpha is observed;
                // timed searches retain shared root PVS at every worker
                // count.
                // A checked root has only a small evasion set, and a
                // shared null-window alpha is both unnecessary and
                // nondeterministic there: concurrent equal evasions can
                // leave the stable-order line as an incomplete scout.
                // Search checked roots with full windows so every legal
                // evasion receives an authoritative score before the
                // stable root ranking is applied.
                const bool use_root_pvs = !multi_pv && !root.in_check() &&
                    (!limits.depth.has_value() || worker_count < 4);
                int aspiration_retry_count = 0;
                for (;;) {
                    SearchStats attempt_stats;
                    bool attempt_aborted = false;
                    bool window_failed_low = false;
                    bool window_failed_high = false;
                    // Every aspiration attempt must rank only lines
                    // searched under its own window.  If a later attempt
                    // is interrupted, retaining `completed` flags from a
                    // prior failed window would make stale bounds look
                    // like current root results.
                    std::fill(lines.begin(), lines.end(), RootLine{});
                    pool.run(root, parallel_moves, depth, root_check_extension,
                             stable_root_indices, lines, use_root_pvs, root_static_eval,
                             window_alpha, window_beta, attempt_stats, attempt_aborted,
                             window_failed_low, window_failed_high);
                    accumulate_stats(iteration_stats, attempt_stats);
                    if (attempt_aborted) {
                        aborted = true;
                        break;
                    }
                    if (!use_aspiration || (!window_failed_low && !window_failed_high)) {
                        break;
                    }

                    aspiration_researched = true;
                    ++iteration_stats.aspiration_researches;
                    ++aspiration_retry_count;
                    if (aspiration_retry_count > 3) {
                        // A pathological score swing gets one final
                        // full-window search so no inexact aspiration line
                        // can become the completed root result.
                        window_alpha = -kInfinity;
                        window_beta = kInfinity;
                        continue;
                    }
                    aspiration_window = std::min(
                        kInfinity / 4, aspiration_window * 2 + 16);
                    if (window_failed_low) {
                        window_alpha = std::max(-kInfinity,
                                                window_alpha - aspiration_window);
                    }
                    if (window_failed_high) {
                        window_beta = std::min(kInfinity,
                                               window_beta + aspiration_window);
                    }
                }
                if (aborted) {
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                &root_context.stats, allow_deep_checked_fallback, &fallback_control,
                                true, true);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    accumulate_stats(iteration_stats, root_context.stats);
                    accumulate_stats(total_stats, iteration_stats);
                    const std::vector<std::size_t> partial_ranked_indices =
                        RootCoordinator::rank(lines);
                    const auto safe_partial = used_short_fallback ?
                        partial_ranked_indices.end() : std::find_if(
                        partial_ranked_indices.begin(), partial_ranked_indices.end(),
                        [&lines, &parallel_moves, &root](const std::size_t index) {
                            return index < parallel_moves.size() &&
                                lines[index].exact &&
                                !root_move_exposes_immediate_check(root, parallel_moves[index]) &&
                                !root_move_is_broad_quiet_check(root, parallel_moves[index]);
                        });
                    if (result.completed_depth == 0 &&
                        safe_partial != partial_ranked_indices.end()) {
                        const RootLine& partial_best = lines[*safe_partial];
                        result.score_cp = partial_best.score;
                        result.mate = mate_from_score(partial_best.score);
                        if (partial_best.pv.length > 0) {
                            result.best_move = partial_best.pv.moves[0];
                            result.pv = partial_best.pv.to_vector();
                        }
                    }
                    break;
                }

                // Use one deterministic ranking path for both single-PV
                // and MultiPV roots. The explicit stable index matters
                // when concurrent workers finish equal-scoring lines in
                // different orders.
                std::vector<std::size_t> ranked_indices =
                    RootCoordinator::rank(lines);
                if (ranked_indices.empty()) {
                    if (result.completed_depth == 0 && !used_short_fallback &&
                        defer_nonchecking_short_fallback) {
                        if (const auto fallback = short_search_fallback_move(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                            fallback.has_value()) {
                            result.best_move = fallback->move;
                            result.pv = {fallback->move};
                            const int fallback_score =
                                normalize_root_fallback_score(fallback->score);
                            result.score_cp = fallback_score;
                            result.mate = mate_from_score(fallback_score);
                            used_short_fallback = true;
                        }
                    }
                    accumulate_stats(iteration_stats, root_context.stats);
                    accumulate_stats(total_stats, iteration_stats);
                    break;
                }

                // Some fixed-depth threaded confirmations deliberately
                // look one or two plies deeper to choose a candidate. A
                // RootLine must not publish that selective score under
                // the shallower completed depth, so retain the ordinary
                // root lines until the selected move is refreshed at the
                // requested horizon.
                std::optional<std::vector<RootLine>> pre_selective_lines;
                bool needs_nominal_root_refresh = false;
                const auto snapshot_before_selective_update = [&]() {
                    if (!pre_selective_lines.has_value()) {
                        pre_selective_lines = lines;
                    }
                };

                // Root-parallel searches otherwise stop at the shallow result when a
                // short timed search finishes depth two. Re-search a small, stable set of
                // quiet moves whose immediate consequence is forcing so that the root gets
                // the same bounded tactical lookahead as the serial path.
                const bool diagnostic_fixed_depth_selective =
                    limits.depth.has_value() && depth == kRootSelectiveDepth;
                const bool short_timed_selective =
                     limits.movetime.has_value() &&
                     ((*limits.movetime >= kShortTimedFallbackMinimum &&
                       *limits.movetime <= kShortTimedSerialThreshold) ||
                      (*limits.movetime < kShortTimedFallbackMinimum &&
                       !root.in_check())) &&
                     depth + 1 == kRootSelectiveDepth;
                if (!multi_pv && !limits.search_moves_specified && !root_context.aborted &&
                    !time_manager.should_stop(root_context.visited_nodes()) &&
                    (diagnostic_fixed_depth_selective || short_timed_selective)) {
                    const std::size_t best_candidate_index = ranked_indices.front();
                    const RootLine& shallow_best = lines[best_candidate_index];
                    const MoveMetadata& best_metadata = parallel_moves[best_candidate_index];
                    if (!best_metadata.is_capture() && !best_metadata.gives_check &&
                        best_metadata.move.promotion() == Promotion::none &&
                        !mate_from_score(shallow_best.score).has_value()) {
                        const PositionFeatures root_features = root.position_features();
                        std::vector<std::pair<std::size_t, MoveMetadata>> candidates;
                        candidates.reserve(3);
                        for (const std::size_t candidate_index : ranked_indices) {
                            if (candidate_index == best_candidate_index ||
                                lines[candidate_index].score <
                                    shallow_best.score - kRootSelectiveMargin ||
                                !lines[candidate_index].completed) {
                                continue;
                            }
                            const MoveMetadata& metadata = parallel_moves[candidate_index];
                            if (metadata.is_capture() || metadata.gives_check ||
                                metadata.move.promotion() != Promotion::none) {
                                continue;
                            }
                            if (!root.make_search_move(metadata)) {
                                continue;
                            }
                            const bool forcing = quiet_move_is_forcing(
                                root_features, root.position_features(), metadata);
                            (void)root.unmake_move();
                            if (!forcing) {
                                continue;
                            }
                            candidates.emplace_back(candidate_index, metadata);
                            if (candidates.size() == 3) {
                                break;
                            }
                        }

                        for (const auto& [candidate_index, metadata] : candidates) {
                            if (time_manager.should_stop(root_context.visited_nodes())) {
                                break;
                            }
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                root_context.selectively_research_root_move(root, metadata,
                                                                            depth + 1);
                            if (root_context.aborted) {
                                break;
                            }
                            if (!verification.authoritative) {
                                continue;
                            }
                            if (verification.score > shallow_best.score +
                                    kRootSelectiveImprovement) {
                                ++root_context.stats.root_selective_researches;
                                snapshot_before_selective_update();
                                lines[candidate_index].score = verification.score;
                                lines[candidate_index].pv = verification.pv;
                                lines[candidate_index].completed = true;
                                lines[candidate_index].exact = true;
                                lines[candidate_index].selective_bound = false;
                                lines[candidate_index].safe_upper_bound = false;
                                lines[candidate_index].selective_upper_bound = false;
                                needs_nominal_root_refresh = true;
                                ranked_indices = RootCoordinator::rank(lines);
                                break;
                            }
                        }
                    }
                }

                // At fixed depth one, a direct check/capture/promotion can be
                // hidden behind a shallow quiet root score.  Re-search only a
                // small near-tied forcing band so root-parallel fixed-depth
                // behavior retains the serial tactical preference without
                // changing the normal timed or deeper search budget.
                const bool threaded_depth_one_forcing_selective =
                    limits.depth.has_value() && depth == 1;
                if (!multi_pv && !limits.search_moves_specified &&
                    threaded_depth_one_forcing_selective && !root_context.aborted &&
                    !time_manager.should_stop(root_context.visited_nodes()) &&
                    !ranked_indices.empty()) {
                    const std::size_t best_candidate_index = ranked_indices.front();
                    const RootLine& shallow_best = lines[best_candidate_index];
                    const MoveMetadata& best_metadata = parallel_moves[best_candidate_index];
                    if (!best_metadata.is_capture() && !best_metadata.gives_check &&
                        best_metadata.move.promotion() == Promotion::none &&
                        !mate_from_score(shallow_best.score).has_value()) {
                        std::vector<std::pair<std::size_t, MoveMetadata>> candidates;
                        candidates.reserve(3);
                        for (const std::size_t candidate_index : ranked_indices) {
                            if (candidate_index == best_candidate_index ||
                                lines[candidate_index].score <
                                    shallow_best.score - kRootSelectiveMargin ||
                                !lines[candidate_index].completed) {
                                continue;
                            }
                            const MoveMetadata& metadata = parallel_moves[candidate_index];
                            if (!metadata.is_capture() && !metadata.gives_check &&
                                metadata.move.promotion() == Promotion::none) {
                                continue;
                            }
                            candidates.emplace_back(candidate_index, metadata);
                            if (candidates.size() == 3) {
                                break;
                            }
                        }

                        if (!candidates.empty()) {
                            // Compare the incumbent at the same horizon as
                            // every forcing alternative.  Comparing a
                            // depth-one quiet score directly with a depth-two
                            // forcing score lets a merely near-tied check
                            // replace a genuinely stronger quiet move.
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification incumbent_verification =
                                root_context.selectively_research_root_move(
                                    root, best_metadata, depth + 1);
                            if (!root_context.aborted && incumbent_verification.authoritative) {
                                const int shallow_best_score = shallow_best.score;
                                int confirmed_score = incumbent_verification.score;
                                std::size_t confirmed_index = best_candidate_index;
                                snapshot_before_selective_update();
                                lines[best_candidate_index].score = incumbent_verification.score;
                                lines[best_candidate_index].pv = incumbent_verification.pv;
                                lines[best_candidate_index].completed = true;
                                lines[best_candidate_index].exact = true;
                                lines[best_candidate_index].selective_bound = false;
                                lines[best_candidate_index].safe_upper_bound = false;
                                lines[best_candidate_index].selective_upper_bound = false;
                                needs_nominal_root_refresh = true;

                                for (const auto& [candidate_index, metadata] : candidates) {
                                    ++root_context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification verification =
                                        root_context.selectively_research_root_move(
                                            root, metadata, depth + 1);
                                    if (root_context.aborted) {
                                        break;
                                    }
                                    if (!verification.authoritative) {
                                        continue;
                                    }
                                    lines[candidate_index].score = verification.score;
                                    lines[candidate_index].pv = verification.pv;
                                    lines[candidate_index].completed = true;
                                    lines[candidate_index].exact = true;
                                    lines[candidate_index].selective_bound = false;
                                    lines[candidate_index].safe_upper_bound = false;
                                    lines[candidate_index].selective_upper_bound = false;
                                    if (verification.score > confirmed_score) {
                                        confirmed_score = verification.score;
                                        confirmed_index = candidate_index;
                                    }
                                }
                                if (!root_context.aborted) {
                                    if (confirmed_index != best_candidate_index ||
                                        confirmed_score != shallow_best_score) {
                                        ++root_context.stats.root_selective_researches;
                                    }
                                    ranked_indices = RootCoordinator::rank(lines);
                                }
                            }
                        }
                    }
                }

                if (needs_nominal_root_refresh) {
                    bool nominal_refresh_completed = false;
                    if (!root_context.aborted &&
                        !time_manager.should_stop(root_context.visited_nodes()) &&
                        !ranked_indices.empty()) {
                        const std::size_t selected_index = ranked_indices.front();
                        if (selected_index < parallel_moves.size()) {
                            ++root_context.stats.root_selective_candidates;
                            const SearchContext::RootVerification verification =
                                root_context.selectively_research_root_move(
                                    root, parallel_moves[selected_index], depth);
                            if (!root_context.aborted && verification.authoritative) {
                                lines[selected_index].score = verification.score;
                                lines[selected_index].pv = verification.pv;
                                lines[selected_index].completed = true;
                                lines[selected_index].exact = true;
                                lines[selected_index].selective_bound = false;
                                lines[selected_index].safe_upper_bound = false;
                                lines[selected_index].selective_upper_bound = false;
                                nominal_refresh_completed = true;
                                if (pre_selective_lines.has_value()) {
                                    // The selective pass may have updated
                                    // several candidates at a deeper
                                    // horizon. Keep only the selected
                                    // candidate's nominal-depth refresh;
                                    // restoring the other lines prevents
                                    // mixed-horizon scores from entering
                                    // ranking, MultiPV publication, or the
                                    // next iteration's schedule.
                                    const RootLine nominal_line = lines[selected_index];
                                    lines = *pre_selective_lines;
                                    lines[selected_index] = nominal_line;
                                    ranked_indices = RootCoordinator::rank(lines);
                                }
                            }
                        }
                    }
                    if (!nominal_refresh_completed && pre_selective_lines.has_value()) {
                        // A selective confirmation that cannot finish its
                        // nominal refresh is discovery-only. Restore the
                        // fully searched root lines before result and info
                        // publication, including the interrupted case.
                        lines = *pre_selective_lines;
                        ranked_indices = RootCoordinator::rank(lines);
                    }
                }

                const std::optional<Move> previous_best_move = result.best_move;
                const std::vector<Move> previous_pv = result.pv;
                const bool had_completed_iteration = result.completed_depth > 0;
                const bool complete_root_coverage = std::all_of(
                    lines.begin(), lines.end(), [](const RootLine& line) {
                        return line.searched;
                    });
                const bool has_exact_incumbent = !ranked_indices.empty() &&
                    lines[ranked_indices.front()].exact;
                const int exact_incumbent_score = has_exact_incumbent ?
                    lines[ranked_indices.front()].score : -kInfinity;
                const bool root_lines_have_safe_provenance = complete_root_coverage &&
                    std::all_of(lines.begin(), lines.end(), [has_exact_incumbent,
                                                             exact_incumbent_score](
                                                                const RootLine& line) {
                        return line.exact || line.safe_upper_bound ||
                            (has_exact_incumbent && line.selective_bound &&
                             line.selective_upper_bound &&
                             line.score <= exact_incumbent_score);
                    });
                const bool authoritative_root_iteration =
                    root_lines_have_safe_provenance && has_exact_incumbent;
                // A complete root pass is publishable even when normal
                // qsearch/selective provenance prevents strict authority.
                // Publication mirrors the serial path: any complete root
                // pass with a valid ranked PV advances the published
                // iteration. Strict provenance is reserved for
                // authoritative_root_iteration, which alone gates
                // aspiration seeding and proof-sensitive metadata.
                const bool publishable_root_iteration = complete_root_coverage &&
                    !ranked_indices.empty() &&
                    lines[ranked_indices.front()].completed &&
                    lines[ranked_indices.front()].pv.length > 0;

                // Remember completed lines for the next root schedule.
                // Selective estimates are useful ordering hints when the
                // iteration also contains an authoritative line.  If an
                // entire attempted iteration is selective, do not replace
                // the previous iteration's queue evidence unless there is
                // no completed iteration yet and the estimate is serving
                // as the initial fallback.
                for (std::size_t index = 0; index < lines.size(); ++index) {
                    if (!lines[index].completed) {
                        continue;
                    }
                    if (!authoritative_root_iteration && had_completed_iteration) {
                        continue;
                    }
                    if (RootScheduleRecord* record =
                            schedule_record_for(parallel_moves[index].move);
                        record != nullptr) {
                        record->previous_score = lines[index].score;
                        record->has_previous_score = true;
                        record->previous_exact = lines[index].exact;
                    }
                }

                const std::size_t best_index = ranked_indices.front();
                const RootLine& best_line = lines[best_index];

                if (publishable_root_iteration) {
                    result.completed_depth = depth;
                    result.score_cp = best_line.score;
                    result.mate = mate_from_score(best_line.score);
                    result.best_move = best_line.pv.length > 0 ?
                        std::optional<Move>{best_line.pv.moves[0]} : result.best_move;
                    if (best_line.pv.length > 0) {
                        result.pv = best_line.pv.to_vector();
                    } else if (result.best_move.has_value()) {
                        result.pv = {*result.best_move};
                    }
                } else if (!had_completed_iteration && best_line.completed) {
                    // Keep a fully returned selective line as a depth-zero
                    // fallback when no authoritative iteration exists yet.
                    // It remains deliberately outside completed-depth and
                    // aspiration authority.
                    result.score_cp = best_line.score;
                    result.mate = mate_from_score(best_line.score);
                    if (best_line.pv.length > 0) {
                        result.best_move = best_line.pv.moves[0];
                        result.pv = best_line.pv.to_vector();
                    }
                }
                const int searched_iteration_score = result.score_cp;
                const bool searched_best_move_changed = had_completed_iteration &&
                    previous_best_move != result.best_move;
                const bool searched_pv_changed = had_completed_iteration &&
                    previous_pv != result.pv;
                bool bounded_fallback_result = false;

                if (authoritative_root_iteration && !multi_pv &&
                    (short_tactical_fallback || defer_nonchecking_short_fallback) &&
                    !defer_expensive_short_fallback &&
                    result.completed_depth == 1 &&
                    (short_capture_needs_fallback(parallel_moves, result.best_move) ||
                     short_quiet_needs_fallback(root, parallel_moves, result.best_move) ||
                     short_quiet_exposes_immediate_check(
                         root, parallel_moves, result.best_move) ||
                     short_check_needs_fallback(parallel_moves, result.best_move) ||
                     short_checked_root_king_evasion_needs_fallback(
                         root, parallel_moves, result.best_move))) {
                    // Keep the post-iteration safety correction bounded;
                    // recursive fallback work is never started after a
                    // short explicit deadline.
                    auto fallback = short_search_fallback_move(
                        root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                        &root_context.stats, allow_deep_checked_fallback,
                        &fallback_control, true, true);
                    std::optional<Move> fallback_move;
                    int fallback_score = best_line.score;
                    if (fallback.has_value()) {
                        fallback_move = fallback->move;
                        fallback_score = normalize_root_fallback_score(fallback->score);
                    }
                    const auto current_metadata = result.best_move.has_value() ?
                        std::find_if(
                            parallel_moves.begin(), parallel_moves.end(),
                            [&result](const MoveMetadata& metadata) {
                                return metadata.move == *result.best_move;
                            }) : parallel_moves.end();
                    if (current_metadata != parallel_moves.end() &&
                        !current_metadata->is_capture() && !current_metadata->gives_check &&
                        current_metadata->move.promotion() == Promotion::none &&
                        (!fallback_move.has_value() ||
                         *fallback_move == current_metadata->move)) {
                        if (const auto checking_capture = scored_safe_checking_capture(
                                root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                allow_deep_checked_fallback, &fallback_control);
                            checking_capture.has_value()) {
                            fallback_move = checking_capture->move;
                            fallback_score = normalize_root_fallback_score(checking_capture->score);
                        } else {
                            fallback_move.reset();
                        }
                    }
                    const bool should_replace = fallback_move.has_value() &&
                        short_fallback_should_replace_completed_root(
                            root, parallel_moves, result.best_move, *fallback_move);
                    if (should_replace) {
                        const std::optional<Move> superseded_move = result.best_move;
                        result.best_move = *fallback_move;
                        result.pv = {*fallback_move};
                        result.score_cp = fallback_score;
                        result.mate = mate_from_score(fallback_score);
                        // The info loop below reads the ranked root line,
                        // not result.pv. Replace that line as well so a
                        // bounded safety correction cannot publish a PV
                        // for the superseded move before bestmove.
                        if (!ranked_indices.empty()) {
                            RootLine& corrected_line = lines[ranked_indices.front()];
                            corrected_line.score = fallback_score;
                            corrected_line.exact = false;
                            corrected_line.selective_bound = false;
                            corrected_line.safe_upper_bound = false;
                            corrected_line.selective_upper_bound = false;
                            corrected_line.pv = {};
                            corrected_line.pv.moves[0] = *fallback_move;
                            corrected_line.pv.length = 1;
                        }
                        if (RootScheduleRecord* record =
                                schedule_record_for(*fallback_move);
                            record != nullptr) {
                            // Keep the next iteration's queue ordering in
                            // sync with the corrected authoritative root
                            // result. Otherwise the old line score would
                            // still make the superseded move look like
                            // the strongest continuation to workers.
                            record->previous_score = fallback_score;
                            record->has_previous_score = true;
                            record->previous_exact = false;
                        }
                        if (superseded_move.has_value() &&
                            *superseded_move != *fallback_move) {
                            if (RootScheduleRecord* record =
                                    schedule_record_for(*superseded_move);
                                record != nullptr) {
                                // The old line was superseded by a
                                // bounded safety decision. Do not let its
                                // stale high score outrank the corrected
                                // move when the next iteration is queued.
                                record->previous_score = -kInfinity;
                                record->has_previous_score = false;
                                record->previous_exact = false;
                            }
                        }
                        bounded_fallback_result = true;
                    }
                }

                accumulate_stats(iteration_stats, root_context.stats);
                accumulate_stats(total_stats, iteration_stats);
                if (root_context.aborted) {
                    break;
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                if (!publishable_root_iteration) {
                    // The attempted pass still consumed real search time,
                    // so use its duration to pace the next attempt while
                    // withholding publication and aspiration state.
                    previous_iteration_elapsed = std::max(
                        std::chrono::milliseconds{1}, elapsed - previous_report_elapsed);
                    if ((!unbounded && depth == maximum_depth) ||
                        time_manager.should_stop_after_iteration()) {
                        break;
                    }
                    continue;
                }

                const std::uint64_t visited = total_stats.nodes + total_stats.qnodes;
                const std::uint64_t nps = elapsed.count() > 0 ? visited * 1000 /
                    static_cast<std::uint64_t>(elapsed.count()) : visited;
                const std::size_t line_count = multi_pv ?
                    std::min(options.multi_pv, ranked_indices.size()) : std::size_t{1};
                for (std::size_t rank = 0; rank < line_count; ++rank) {
                    const RootLine& line = lines[ranked_indices[rank]];
                    SearchInfo info{depth, line.score, mate_from_score(line.score), visited,
                                    nps, elapsed, line.pv.to_vector()};
                    info.seldepth = total_stats.seldepth;
                    info.qnodes = total_stats.qnodes;
                    info.tt_hits = total_stats.tt_hits;
                    info.tbhits = total_stats.tbhits;
                    info.multipv = static_cast<int>(rank + 1);
                    safely_report_info(sink, info);
                }
                const auto iteration_elapsed = elapsed - previous_report_elapsed;
                const int iteration_observation_score = bounded_fallback_result ?
                    searched_iteration_score : result.score_cp;
                time_manager.observe_iteration(SearchIterationObservation{
                    depth,
                    // Selective root confirmation is a full-window result
                    // and remains part of pacing. A bounded fallback is a
                    // caller-facing safety choice, so use the ordinary
                    // root line for hardness and aspiration state.
                    iteration_observation_score,
                    bounded_fallback_result ? searched_best_move_changed :
                        (had_completed_iteration && previous_best_move != result.best_move),
                    bounded_fallback_result ? searched_pv_changed :
                        (had_completed_iteration && previous_pv != result.pv),
                    aspiration_researched,
                    visited});
                result.timing = time_manager.diagnostics();
                if (authoritative_root_iteration) {
                    previous_score = iteration_observation_score;
                } else {
                    // A complete selective root pass is a valid published
                    // iteration, but its score is not a safe aspiration
                    // center. Force a fresh full-window baseline next.
                    previous_score.reset();
                }
                previous_iteration_elapsed = iteration_elapsed;
                previous_report_elapsed = elapsed;
                if ((!unbounded && depth == maximum_depth) ||
                    time_manager.should_stop_after_iteration()) {
                    break;
                }
            }
            result.stats = total_stats;
        }

        if (limits.ponder && (legal_moves.empty() || root_is_forced_draw)) {
            // A terminal ponder root has nothing to deepen; it answers as soon
            // as the GUI stops it or converts the ponder with a ponderhit.
            session->wait_until_stopped_or_ponderhit();
            (void)session->take_ponderhit_limits();
        }
        if (result.completed_depth == 0 && result.best_move.has_value()) {
            // Retain the emergency line for non-UCI search consumers that
            // use the final info callback to keep the published PV in sync
            // with bestmove. The UCI controller filters this depth-zero
            // diagnostic because UCI info depths are positive iterations.
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            const std::uint64_t visited = result.stats.nodes + result.stats.qnodes;
            const std::uint64_t nps = elapsed.count() > 0 ? visited * 1000 /
                static_cast<std::uint64_t>(elapsed.count()) : visited;
            result.pv = {*result.best_move};
            SearchInfo info{0, result.score_cp, mate_from_score(result.score_cp), visited,
                            nps, elapsed, result.pv};
            info.seldepth = result.stats.seldepth;
            info.qnodes = result.stats.qnodes;
            info.tt_hits = result.stats.tt_hits;
            info.tbhits = result.stats.tbhits;
            safely_report_info(sink, info);
        }
        if (lazy_pool != nullptr) {
            // Helpers never publish a result; their only contribution is the
            // transposition-table traffic they generated and the node counts
            // merged here.
            lazy_pool->stop();
            accumulate_stats(result.stats, lazy_pool->stats());
        }
        result.timing = time_manager.diagnostics();
    } catch (...) {
        result.failed = true;
        try {
                    if (!result.best_move.has_value()) {
                        const std::vector<Move> legal = root.legal_moves();
                for (const Move& move : legal) {
                    if (!limits.search_moves_specified ||
                        std::find(limits.search_moves.begin(), limits.search_moves.end(), move) !=
                            limits.search_moves.end()) {
                            result.best_move = move;
                            result.pv = {move};
                            break;
                    }
                }
            }
        } catch (...) {
        }
    }

    result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    result.mate = mate_from_score(result.score_cp);
    result.completed = true;
    result.cancelled = session->stop_requested().load(std::memory_order_relaxed);
    (void)session->publish_completion(sink, result);
}

} // namespace koi::detail
