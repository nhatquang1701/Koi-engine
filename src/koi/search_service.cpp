#include "koi/search_service.hpp"

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

#include "koi/detail/search_ordering.hpp"
#include "koi/detail/root_coordinator.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/search_session.hpp"
#include "koi/syzygy_tablebase.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi {
namespace {

constexpr int kInfinity = 1'000'000;
constexpr int kMateScore = 100'000;
constexpr int kMateThreshold = 99'000;
constexpr int kMaximumSearchDepth = 64;
constexpr int kMaximumQuiescenceDepth = 16;
constexpr int kMaximumQuiescenceSafetyDepth = 64;
// A checking extension must not preserve the same effective depth through an
// alternating checking sequence. Two extensions retain tactical coverage,
// while a finite per-path budget prevents perpetual-check trees from growing
// the recursive stack without consuming search depth.
constexpr int kMaximumCheckExtensionsPerPath = 2;
// Keep a bounded quiet-check horizon so forcing check sequences can resolve
// one additional interposition without turning quiescence into an unbounded
// checking search. Checked positions still use the separate safety cap.
constexpr int kMaximumQuiescenceCheckDepth = 3;
// Continue generating candidates for narrow deep quiet checks beyond the
// ordinary horizon. The later filter below still rejects broad queen/rook
// checks, but it cannot rescue a check that was never generated.
constexpr int kNarrowQuietCheckProbeStartDepth = 6;
constexpr int kMaximumQuiescenceNarrowQuietCheckDepth = 7;
constexpr int kAspirationWindow = 50;
constexpr int kRootSelectiveDepth = 3;
constexpr int kRootSelectiveMargin = 20;
constexpr int kRootSelectiveImprovement = 15;
constexpr int kRootKingSafetySelectiveDepth = 5;
constexpr int kRootKingSafetyTieMargin = 50;
constexpr std::size_t kMaximumRootKingSafetyCandidates = 2;
constexpr int kIncompleteRootForcingMargin = 50;
// If a checked-root search is interrupted before any iteration completes,
// static material can make a king capture look much better than a safer
// escape.  Treat king-zone exposure as a crisis tie-break, but only inside
// this emergency path and only when the raw scores are reasonably close.
constexpr int kEmergencyKingEscapeSafetyMargin = 650;
constexpr int kEmergencyKingZoneAttackWeight = 400;
constexpr int kEmergencyKingCaptureRisk = 160;
constexpr int kEmergencyQuietForcingMargin = 200;
constexpr int kEmergencyBroadCheckReplacementMargin = 200;
constexpr int kEmergencySafeCaptureTieMargin = 32;
constexpr int kEmergencySafeExchangeTieMargin = 80;
// In an interrupted root, an irreversible quiet pawn push that leaves an
// immediate checking resource is more dangerous than a similarly scored
// piece move. Keep this discount local to the unsafe fallback ranking; normal
// completed search and ordinary pawn evaluation are unchanged.
constexpr int kEmergencyUnsafePawnQuietPenalty = 75;
constexpr std::size_t kMaximumEmergencyForcingCheckEvasions = 3;
constexpr std::size_t kMaximumEmergencyQuietForcingResearches = 16;
// Root workers have a fixed startup/coordination cost.  For short tactical
// roots that cost can consume the whole first iteration, leaving only the
// initial ordered move when the clock expires.  Keep quiet roots parallel, but
// give forcing roots enough room to complete an authoritative serial iteration.
constexpr auto kShortTimedSerialThreshold = std::chrono::milliseconds{500};
// The emergency static fallback is deliberately limited to the middle of the
// short-search band. At very low requested movetime the fixed overhead/safety
// ceiling leaves too little budget for both fallback scoring and a root pass;
// at the serial-path cutoff, scoring every root move can likewise starve the
// first authoritative iteration.
constexpr auto kShortTimedFallbackMinimum = std::chrono::milliseconds{150};
constexpr auto kShortTimedFallbackThreshold = std::chrono::milliseconds{300};
constexpr std::size_t kMaximumMultiPv = 16;
constexpr std::size_t kNullMoveSparsePieceLimit = 8;
constexpr std::uint16_t kNullMoveRuleSafetyHalfmoves = 90;

std::optional<int> mate_from_score(int score) noexcept {
    if (score >= kMateThreshold) {
        return (kMateScore - score + 1) / 2;
    }
    if (score <= -kMateThreshold) {
        return -((kMateScore + score + 1) / 2);
    }
    return std::nullopt;
}

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

constexpr int piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return 100;
    case PieceType::knight:
        return 320;
    case PieceType::bishop:
        return 330;
    case PieceType::rook:
        return 500;
    case PieceType::queen:
        return 900;
    case PieceType::king:
        return 20'000;
    case PieceType::none:
        return 0;
    }
    return 0;
}

bool piece_attacks_square(const PositionFeatures& features, std::uint8_t source,
                          std::uint8_t target) noexcept {
    if (source >= 64 || target >= 64 || source == target) {
        return false;
    }
    const Piece piece = features.board[source];
    if (piece.empty()) {
        return false;
    }

    const int source_file = source % 8;
    const int source_rank = source / 8;
    const int target_file = target % 8;
    const int target_rank = target / 8;
    const int file_delta = target_file - source_file;
    const int rank_delta = target_rank - source_rank;
    const int abs_file_delta = std::abs(file_delta);
    const int abs_rank_delta = std::abs(rank_delta);

    switch (piece.type) {
    case PieceType::pawn:
        return rank_delta == (piece.color == Color::white ? 1 : -1) && abs_file_delta == 1;
    case PieceType::knight:
        return (abs_file_delta == 1 && abs_rank_delta == 2) ||
               (abs_file_delta == 2 && abs_rank_delta == 1);
    case PieceType::king:
        return abs_file_delta <= 1 && abs_rank_delta <= 1;
    case PieceType::bishop:
    case PieceType::rook:
    case PieceType::queen:
        break;
    case PieceType::none:
        return false;
    }

    const bool diagonal = abs_file_delta == abs_rank_delta && abs_file_delta != 0;
    const bool orthogonal = (file_delta == 0) != (rank_delta == 0);
    if ((piece.type == PieceType::bishop && !diagonal) ||
        (piece.type == PieceType::rook && !orthogonal) ||
        (piece.type == PieceType::queen && !diagonal && !orthogonal)) {
        return false;
    }

    const int file_step = file_delta == 0 ? 0 : (file_delta > 0 ? 1 : -1);
    const int rank_step = rank_delta == 0 ? 0 : (rank_delta > 0 ? 1 : -1);
    for (int file = source_file + file_step, rank = source_rank + rank_step;
         file != target_file || rank != target_rank; file += file_step, rank += rank_step) {
        if (!features.board[static_cast<std::size_t>(rank * 8 + file)].empty()) {
            return false;
        }
    }
    return true;
}

bool quiet_move_is_forcing(const PositionFeatures& before,
                           const PositionFeatures& after_features,
                           const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const PositionFeatures& features = after_features;
    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    if (features.king_zone_attacks[enemy] > before.king_zone_attacks[enemy]) {
        // A quiet move which opens lines or adds pressure around the enemy
        // king is a forcing continuation even if it is not an immediate check.
        return true;
    }
    const std::uint8_t destination = metadata.move.to().index();
    if (destination >= 64) {
        return false;
    }

    int attacked_valuable_pieces = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece target = features.board[square];
        if (target.empty() || target.type == PieceType::king ||
            (target.color == Color::white ? 0U : 1U) != enemy ||
            !piece_attacks_square(features, destination, square)) {
            continue;
        }
        if (target.type == PieceType::queen || target.type == PieceType::rook) {
            return true;
        }
        if (target.type == PieceType::pawn) {
            const int target_file = square % 8;
            const int target_rank = square / 8;
            if (target_file >= 2 && target_file <= 5 &&
                (target_rank == 3 || target_rank == 4)) {
                // An attack on an advanced central pawn is a forcing
                // consequence even when the capture itself is deferred.
                return true;
            }
        }
        if (++attacked_valuable_pieces >= 2) {
            return true;
        }
    }

    const std::uint64_t newly_attacked = features.attacked_squares[own] &
        ~before.attacked_squares[own];
    for (std::uint8_t square = 0; square < 64; ++square) {
        if ((newly_attacked & (std::uint64_t{1} << square)) == 0) {
            continue;
        }
        const Piece target = features.board[square];
        if (!target.empty() && (target.color == Color::white ? 0U : 1U) == enemy &&
            (target.type == PieceType::queen || target.type == PieceType::rook ||
             target.type == PieceType::bishop || target.type == PieceType::knight)) {
            return true;
        }
    }

    const Piece moved = features.board[destination];
    if (moved.type == PieceType::pawn && (moved.color == Color::white ? 0U : 1U) == own) {
        const int rank = destination / 8;
        const int file = destination % 8;
        const int direction = moved.color == Color::white ? 1 : -1;
        bool passed = true;
        for (int candidate_file = std::max(0, file - 1);
             candidate_file <= std::min(7, file + 1); ++candidate_file) {
            for (int candidate_rank = rank + direction;
                 candidate_rank >= 0 && candidate_rank < 8;
                 candidate_rank += direction) {
                const Piece candidate = features.board[
                    static_cast<std::size_t>(candidate_rank * 8 + candidate_file)];
                if (candidate.type == PieceType::pawn &&
                    (candidate.color == Color::white ? 0U : 1U) == enemy) {
                    passed = false;
                }
            }
        }
        const bool advanced = moved.color == Color::white ? rank >= 4 : rank <= 3;
        const bool central_break = (file == 3 || file == 4) &&
            (rank == 3 || rank == 4);
        if (passed && advanced) {
            return true;
        }
        if (central_break) {
            return true;
        }
    }
    return false;
}

bool quiet_move_has_direct_forcing_target(const PositionFeatures& before,
                                          const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const std::uint8_t source = metadata.move.from().index();
    const std::uint8_t destination = metadata.move.to().index();
    if (source >= 64 || destination >= 64 || source == destination) {
        return false;
    }

    const Piece moving = before.board[source];
    if (moving.empty()) {
        return false;
    }

    PositionFeatures projected = before;
    projected.board[source] = {};
    projected.board[destination] = moving;
    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    int attacked_minor_pieces = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece target = projected.board[square];
        if (target.empty() || target.type == PieceType::king ||
            (target.color == Color::white ? 0U : 1U) != enemy ||
            !piece_attacks_square(projected, destination, square)) {
            continue;
        }
        if (target.type == PieceType::queen || target.type == PieceType::rook) {
            return true;
        }
        if (target.type == PieceType::pawn) {
            const int target_file = square % 8;
            const int target_rank = square / 8;
            if (target_file >= 2 && target_file <= 5 &&
                (target_rank == 3 || target_rank == 4)) {
                return true;
            }
        }
        if (target.type == PieceType::bishop || target.type == PieceType::knight) {
            ++attacked_minor_pieces;
            if (attacked_minor_pieces >= 2) {
                return true;
            }
        }
    }
    return false;
}

bool quiet_move_has_king_ring_target(const PositionFeatures& before,
                                     const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const std::uint8_t source = metadata.move.from().index();
    const std::uint8_t destination = metadata.move.to().index();
    if (source >= 64 || destination >= 64 || source == destination) {
        return false;
    }

    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    const std::uint8_t enemy_king = before.king_squares[enemy].index();
    if (enemy_king >= 64) {
        return false;
    }

    const Piece moving = before.board[source];
    if (moving.empty()) {
        return false;
    }

    PositionFeatures projected = before;
    projected.board[source] = {};
    projected.board[destination] = moving;
    const int king_file = enemy_king % 8;
    const int king_rank = enemy_king / 8;
    for (int file = king_file - 1; file <= king_file + 1; ++file) {
        for (int rank = king_rank - 1; rank <= king_rank + 1; ++rank) {
            if (file < 0 || file >= 8 || rank < 0 || rank >= 8 ||
                (file == king_file && rank == king_rank)) {
                continue;
            }
            if (piece_attacks_square(projected, destination,
                                     static_cast<std::uint8_t>(rank * 8 + file))) {
                return true;
            }
        }
    }
    return false;
}

bool quiet_move_has_pawn_break_target(const MoveMetadata& metadata) noexcept {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none ||
        metadata.moving_piece != PieceType::pawn) {
        return false;
    }

    const std::uint8_t destination = metadata.move.to().index();
    if (destination >= 64) {
        return false;
    }
    const int file = destination % 8;
    const int rank = destination / 8;
    return (file == 3 || file == 4) && (rank == 3 || rank == 4);
}

bool null_move_is_safe(const GameState& state, const PositionFeatures& features) noexcept {
    if (features.game_phase < 8 ||
        !state.has_non_pawn_material(state.side_to_move()) ||
        !state.has_non_pawn_material(opposite(state.side_to_move())) ||
        state.halfmove_clock() >= kNullMoveRuleSafetyHalfmoves ||
        state.is_repetition_sensitive()) {
        return false;
    }

    // Sparse positions contain too little tactical reserve for the null-move
    // assumption to be dependable, even when their phase value remains high
    // because the few remaining pieces are major pieces.
    return state.tablebase_snapshot().piece_count() > kNullMoveSparsePieceLimit;
}

bool deep_quiet_check_candidate(const MoveMetadata& metadata) noexcept {
    // The third checking layer is deliberately selective. Minor-piece and
    // pawn checks are usually forcing forks, discovered attacks, or promotion
    // races with a small reply set; queen/rook checking continuations create
    // a very wide tree and remain covered at the normal two-ply horizon.
    return metadata.gives_check && !metadata.is_capture() &&
        metadata.move.promotion() == Promotion::none &&
        (metadata.moving_piece == PieceType::pawn ||
         metadata.moving_piece == PieceType::knight ||
         metadata.moving_piece == PieceType::bishop);
}

bool narrow_deep_quiet_check_candidate(GameState& state, const MoveMetadata& metadata) {
    if (deep_quiet_check_candidate(metadata)) {
        return true;
    }
    if (!metadata.gives_check || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none ||
        (metadata.moving_piece != PieceType::queen &&
         metadata.moving_piece != PieceType::rook)) {
        return false;
    }

    // Queen and rook checks are normally too broad to carry through the third
    // quiescence layer.  Keep them when the checked side has only a narrow
    // set of legal evasions; this catches short mating nets without opening a
    // full queen-check tree at every shallow leaf.
    if (!state.make_search_move(metadata)) {
        return false;
    }
    const bool narrow_evasion_set = state.legal_moves().size() <= 3;
    (void)state.unmake_move();
    return narrow_evasion_set;
}

using detail::PrincipalVariation;
using detail::RootCoordinator;
using detail::RootLine;

void accumulate_stats(SearchStats& total, const SearchStats& partial) noexcept {
    total.nodes += partial.nodes;
    total.qnodes += partial.qnodes;
    total.position_feature_extractions += partial.position_feature_extractions;
    total.lmr_parent_feature_reuses += partial.lmr_parent_feature_reuses;
    total.evaluation_cache_hits += partial.evaluation_cache_hits;
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
    total.quiet_futility_prunes += partial.quiet_futility_prunes;
    total.razoring_prunes += partial.razoring_prunes;
    total.quiet_history_updates += partial.quiet_history_updates;
    total.continuation_history_updates += partial.continuation_history_updates;
    total.tbhits += partial.tbhits;
    total.seldepth = std::max(total.seldepth, partial.seldepth);
}

bool root_move_exposes_immediate_check(const GameState& root,
                                       const MoveMetadata& metadata) noexcept {
    try {
        GameState after_move = root;
        if (!after_move.make_search_move(metadata) || after_move.in_check()) {
            return false;
        }

        MoveMetadataList opponent_moves;
        after_move.legal_moves_with_metadata(
            opponent_moves, true, false, CheckFlagMode::all_moves);
        return std::any_of(opponent_moves.begin(), opponent_moves.end(),
                           [](const MoveMetadata& opponent_move) {
                               return opponent_move.gives_check;
                           });
    } catch (...) {
        return false;
    }
}

int short_fallback_evaluate(const Evaluator& evaluator, const GameState& state,
                            const Color root_color, std::mutex* evaluator_mutex) {
    if (evaluator_mutex != nullptr) {
        std::lock_guard lock(*evaluator_mutex);
        return evaluator.evaluate(state, root_color);
    }
    return evaluator.evaluate(state, root_color);
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
    MoveMetadataList opponent_moves;
    after_move.legal_moves_with_metadata(
        opponent_moves, true, false, CheckFlagMode::all_moves);
    if (opponent_moves.empty()) {
        return after_move.in_check() ? -kMateScore + 2 : 0;
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

        MoveMetadataList responses;
        after_opponent.legal_moves_with_metadata(
            responses, true, false, CheckFlagMode::all_moves);
        if (responses.empty()) {
            worst_score = std::min(
                worst_score, after_opponent.in_check() ? -kMateScore + 2 : 0);
            found_reply = true;
            continue;
        }

        int best_response_score = -kInfinity;
        for (const MoveMetadata& response : responses) {
            if (control != nullptr && control->interrupted()) {
                return -kInfinity;
            }
            GameState after_response = after_opponent;
            if (!after_response.make_search_move(response)) {
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
    return found_reply ? worst_score : -kInfinity;
}

int checked_root_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex,
    const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
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
    return found_evasion ? worst_evasion_score : -kInfinity;
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
    MoveMetadataList evasions;
    after_check.legal_moves_with_metadata(
        evasions, true, true, CheckFlagMode::all_moves);
    if (evasions.empty()) {
        return after_check.in_check() ? kMateScore - 1 : 0;
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
            evasion_score = kMateScore - 2;
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return best_evasion_score;
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
            int evasion_score = short_fallback_evaluate(
                evaluator, after_check_evasion, root_color, evaluator_mutex);
            if (after_check_evasion.in_check() &&
                after_check_evasion.legal_moves().empty()) {
                evasion_score = kMateScore - 2;
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
    return found_check ? worst_score - king_exposure_penalty : static_score;
}

int checked_reply_fallback_score(const GameState& after_check, const Color root_color,
                                 const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
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
                const int forcing_score = forcing_reply_fallback_score(
                    after_forcing_reply, root_color, evaluator, evaluator_mutex,
                    opponent_move.captured_piece, control);
                evasion_score = std::min(evasion_score, forcing_score);
            }
        }
        best_evasion_score = std::max(best_evasion_score, evasion_score);
    }
    return best_evasion_score;
}

int overdue_check_reply_fallback_score(
    const GameState& after_check, const Color root_color,
    const Evaluator& evaluator, std::mutex* evaluator_mutex) {
    constexpr std::size_t kMaximumEvasions = 2;
    constexpr std::size_t kMaximumForcingReplies = 1;

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

        int evasion_score = short_fallback_evaluate(
            evaluator, after_evasion, root_color, evaluator_mutex);
        if (after_evasion.in_check()) {
            if (after_evasion.legal_moves().empty()) {
                evasion_score = -kMateScore + 2;
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
            int reply_score = forcing_reply_fallback_score(
                after_reply, root_color, evaluator, evaluator_mutex,
                reply.captured_piece, nullptr);
            if (after_reply.in_check() && after_reply.legal_moves().empty()) {
                reply_score = -kMateScore + 2;
            } else if (reply.is_capture() &&
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
    return best_evasion_score;
}

int forcing_reply_fallback_score(const GameState& after_forcing_reply,
                                 const Color root_color, const Evaluator& evaluator,
                                 std::mutex* evaluator_mutex,
                                 const PieceType captured_piece,
                                 const FallbackControl* control = nullptr) {
    if (control != nullptr && control->interrupted()) {
        return -kInfinity;
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
    const int forcing_loss_floor = captured_piece == PieceType::queen ? -1'200 :
        captured_piece == PieceType::rook ? -650 :
        (captured_piece == PieceType::bishop || captured_piece == PieceType::knight) ? -425 :
        -kInfinity;
    best_score = std::min(best_score, forcing_loss_floor);
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
        int response_score = short_fallback_evaluate(
            evaluator, after_response, root_color, evaluator_mutex);
        if (response.is_capture() && captured_piece == PieceType::none &&
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
                response_score = std::min(
                    response_score,
                    short_fallback_evaluate(
                        evaluator, after_counter, root_color, evaluator_mutex));
            }
        }
        if (after_response.in_check() && after_response.legal_moves().empty()) {
            response_score = kMateScore - 2;
        }
        best_score = std::max(best_score, response_score);
    }
    return best_score;
}

std::optional<Move> first_safe_short_search_move(
    const GameState& root, const MoveMetadataList& legal_moves) noexcept {
    try {
        for (const MoveMetadata& metadata : legal_moves) {
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
    const MoveMetadataList& legal_moves,
    const std::optional<Move>& candidate) noexcept;
bool quiet_move_leaves_safe_capture(
    const MoveMetadata& candidate, const MoveMetadataList& opponent_moves) noexcept;

std::optional<Move> first_safe_checking_capture(
    const MoveMetadataList& legal_moves) noexcept {
    for (const MoveMetadata& metadata : legal_moves) {
        if (metadata.is_capture() && metadata.gives_check) {
            return metadata.move;
        }
    }
    return std::nullopt;
}

std::optional<Move> short_search_fallback_move(
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
            bool hanging_major_check = false;
            if (after_move.in_check()) {
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
                        checked_reply_fallback_score(
                            after_move, root.side_to_move(), evaluator, evaluator_mutex, nullptr);
                } else if (metadata.moving_piece == PieceType::king) {
                    score = king_evasion_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex,
                        metadata.is_capture(), control);
                } else {
                    score = checked_reply_fallback_score(
                        after_move, root.side_to_move(), evaluator, evaluator_mutex, control);
                }
            } else {
                score = short_fallback_evaluate(
                    evaluator, after_move, root.side_to_move(), evaluator_mutex);
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
            if (!after_move.in_check() && perform_deep_fallback &&
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
            } else if (!after_move.in_check() && permit_overdue_shallow &&
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
                const bool major_check = metadata.moving_piece == PieceType::queen ||
                    metadata.moving_piece == PieceType::rook;
                if (major_check && (!broad_check_fallback_move.has_value() ||
                    score > broad_check_fallback_score)) {
                    broad_check_fallback_move = metadata.move;
                    broad_check_fallback_score = score;
                    broad_check_fallback_is_major = true;
                }
                continue;
            }
            if (exposes_immediate_check && !root.in_check() &&
                !safe_equal_non_pawn_exchange && !safe_non_pawn_capture) {
                if (!first_unsafe_quiet_move.has_value() && !metadata.is_capture() &&
                    metadata.move.promotion() == Promotion::none) {
                    first_unsafe_quiet_move = metadata.move;
                }
                const int unsafe_score = score -
                    (metadata.moving_piece == PieceType::pawn && !metadata.is_capture() ?
                         kEmergencyUnsafePawnQuietPenalty : 0);
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
        return best_quiet_forcing_move;
    }
    if (!best_move.has_value() && best_safe_pawn_capture.has_value() &&
        first_unsafe_quiet_move.has_value()) {
        // If every positive-SEE pawn capture was quarantined and all useful
        // quiet alternatives expose immediate checks, retain stable move
        // ordering instead of promoting the least-bad unsafe quiet score.
        return first_unsafe_quiet_move;
    }
    if (unsafe_best_move.has_value() &&
        (unsafe_best_is_quiet || best_is_hanging_major_check) &&
        (!best_move.has_value() ||
         unsafe_best_score >= best_score + kEmergencyQuietForcingMargin)) {
        if (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
            broad_check_fallback_score >= unsafe_best_score -
                kEmergencyBroadCheckReplacementMargin) {
            return broad_check_fallback_move;
        }
        return unsafe_best_move;
    }
    if (broad_check_fallback_move.has_value() && broad_check_fallback_is_major &&
        (!best_move.has_value() ||
         (!best_is_safe_capture && !best_is_safe_forcing_capture && !best_is_forcing_check &&
          broad_check_fallback_score >= best_score - kEmergencyBroadCheckReplacementMargin))) {
        return broad_check_fallback_move;
    }
    return best_move.has_value() ? best_move : unsafe_best_move;
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
    const MoveMetadataList& legal_moves,
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
                       [candidate](const MoveMetadata& move) {
                           return move.move != *candidate &&
                               (is_safe_material_capture(move) ||
                                (move.is_capture() && move.gives_check));
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

struct SearchContext {
    struct RootMoveScore {
        Move move = Move::no_move();
        int score = -kInfinity;
    };

    struct RootVerification {
        int score = -kInfinity;
        PrincipalVariation pv;
    };

    struct EvaluationCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
    };

    static constexpr std::size_t kEvaluationCacheSize = 8 * 1024;

    const Evaluator& evaluator;
    TranspositionTable& table;
    TimeManager& time_manager;
    std::atomic_bool& stop_requested;
    std::atomic<std::uint64_t>* global_nodes = nullptr;
    std::mutex* evaluator_mutex = nullptr;
    const MoveMetadataList* root_moves = nullptr;
    bool use_transposition_table = true;
    std::atomic_bool* iteration_aborted = nullptr;
    SearchOptions::QuietHistorySideHook quiet_history_side_hook;
    detail::SearchMoveOrdering ordering;
    SearchStats stats;
    bool aborted = false;
    bool allow_root_forcing_extension = false;
    int quiescence_check_depth_limit = kMaximumQuiescenceCheckDepth;
    detail::SearchStack stack;
    std::unique_ptr<EvaluationCacheEntry[]> evaluation_cache;
    std::array<RootMoveScore, kMaximumLegalMoves> root_move_scores{};
    std::size_t root_move_score_count = 0;

    SearchContext(const Evaluator& evaluator, TranspositionTable& table, TimeManager& time_manager,
                  std::atomic_bool& stop_requested,
                  std::atomic<std::uint64_t>* global_nodes = nullptr,
                  std::mutex* evaluator_mutex = nullptr,
                  const MoveMetadataList* root_moves = nullptr,
                  bool use_transposition_table = true,
                  SearchOptions::QuietHistorySideHook quiet_history_side_hook = {})
        : evaluator(evaluator), table(table), time_manager(time_manager), stop_requested(stop_requested),
          global_nodes(global_nodes), evaluator_mutex(evaluator_mutex), root_moves(root_moves),
          use_transposition_table(use_transposition_table),
          evaluation_cache(std::make_unique<EvaluationCacheEntry[]>(kEvaluationCacheSize)),
          quiet_history_side_hook(std::move(quiet_history_side_hook)) {
        if (const auto budget = time_manager.time_budget(); budget.has_value() &&
            *budget < kShortTimedFallbackMinimum) {
            // A sub-150 ms search must finish its first root iteration before
            // spending time on the third quiet-checking layer. Fixed-depth
            // searches remain on the full horizon.
            quiescence_check_depth_limit = kMaximumQuiescenceCheckDepth - 1;
        }
    }

    [[nodiscard]] Color history_side(Color candidate, const bool after_unmake) const {
        if (!quiet_history_side_hook) {
            return candidate;
        }
        // This hook is test/diagnostic-only and can run on every root worker.
        // A diagnostic callback must never be able to terminate the search.
        try {
            return quiet_history_side_hook(candidate, after_unmake);
        } catch (...) {
            return candidate;
        }
    }

    void begin_iteration(std::atomic_bool* shared_abort) noexcept {
        stats = {};
        aborted = false;
        iteration_aborted = shared_abort;
        root_move_score_count = 0;
        stack.reset();
    }

    [[nodiscard]] std::optional<RootMoveScore> best_completed_root_move(
        const GameState& root, const bool avoid_check_exposure) const noexcept {
        if (root_move_score_count == 0) {
            return std::nullopt;
        }
        const auto metadata_for = [this](const Move move) -> const MoveMetadata* {
            if (root_moves == nullptr) {
                return nullptr;
            }
            const auto match = std::find_if(root_moves->begin(), root_moves->end(),
                                            [move](const MoveMetadata& metadata) {
                                                return metadata.move == move;
                                            });
            return match == root_moves->end() ? nullptr : &*match;
        };
        const auto is_losing_capture = [&metadata_for](const Move move) {
            const MoveMetadata* metadata = metadata_for(move);
            return metadata != nullptr && metadata->is_capture() && metadata->see_computed &&
                metadata->see_score < 0 && !metadata->gives_check;
        };
        const auto is_forcing = [&metadata_for](const Move move) {
            const MoveMetadata* metadata = metadata_for(move);
            return metadata != nullptr &&
                (metadata->is_capture() || metadata->gives_check ||
                 metadata->move.promotion() != Promotion::none);
        };
        std::optional<RootMoveScore> best;
        for (std::size_t index = 0; index < root_move_score_count; ++index) {
            const RootMoveScore candidate = root_move_scores[index];
            const MoveMetadata* candidate_metadata = metadata_for(candidate.move);
            if (avoid_check_exposure && candidate_metadata != nullptr &&
                root_move_exposes_immediate_check(root, *candidate_metadata)) {
                continue;
            }
            if (is_losing_capture(candidate.move)) {
                continue;
            }
            if (!best.has_value()) {
                best = candidate;
                continue;
            }
            const RootMoveScore current_best = *best;
            const bool candidate_forcing = is_forcing(candidate.move);
            const bool best_forcing = is_forcing(current_best.move);
            if (best_forcing && !candidate_forcing &&
                candidate.score <= current_best.score + kIncompleteRootForcingMargin) {
                continue;
            }
            if (candidate.score > current_best.score ||
                (candidate.score >= current_best.score - kIncompleteRootForcingMargin &&
                 candidate_forcing && !best_forcing)) {
                best = candidate;
            }
        }
        return best;
    }

    [[nodiscard]] RootVerification selectively_research_root_move(
        GameState& root, const MoveMetadata& metadata, int depth) {
        MoveMetadataList single_move;
        (void)single_move.push_back(metadata);
        const MoveMetadataList* saved_root_moves = root_moves;
        const bool saved_use_transposition_table = use_transposition_table;
        root_moves = &single_move;
        // The first shallow root pass stores exact child entries at the same
        // depth that this verification is meant to extend.  Reusing those
        // entries would make the "deeper" comparison identical to the
        // shallow score and hide the tactical consequence being tested.
        use_transposition_table = false;
        RootVerification verification;
        try {
            verification.score = negamax(root, depth, -kInfinity, kInfinity, 0,
                                         verification.pv);
        } catch (...) {
            root_moves = saved_root_moves;
            use_transposition_table = saved_use_transposition_table;
            throw;
        }
        root_moves = saved_root_moves;
        use_transposition_table = saved_use_transposition_table;
        return verification;
    }

    void request_abort() noexcept {
        aborted = true;
        if (iteration_aborted != nullptr) {
            iteration_aborted->store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t visited_nodes() const noexcept {
        if (global_nodes != nullptr) {
            return global_nodes->load(std::memory_order_relaxed);
        }
        return stats.nodes + stats.qnodes;
    }

    [[nodiscard]] bool reserve_global_node() noexcept {
        if (global_nodes == nullptr) {
            if (const std::optional<std::uint64_t> limit = time_manager.node_limit(); limit.has_value() &&
                stats.nodes + stats.qnodes >= *limit) {
                return false;
            }
            return true;
        }

        const std::optional<std::uint64_t> limit = time_manager.node_limit();
        if (!limit.has_value()) {
            global_nodes->fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        std::uint64_t observed = global_nodes->load(std::memory_order_relaxed);
        for (;;) {
            if (observed >= *limit) {
                return false;
            }
            if (global_nodes->compare_exchange_weak(observed, observed + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool interrupted() noexcept {
        if (stop_requested.load(std::memory_order_relaxed) ||
            (iteration_aborted != nullptr && iteration_aborted->load(std::memory_order_relaxed))) {
            aborted = true;
            return true;
        }
        if (time_manager.should_stop(visited_nodes())) {
            request_abort();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool count_node(bool quiescence) noexcept {
        if (stop_requested.load(std::memory_order_relaxed) ||
            (iteration_aborted != nullptr && iteration_aborted->load(std::memory_order_relaxed))) {
            aborted = true;
            return false;
        }
        if (!reserve_global_node()) {
            request_abort();
            return false;
        }

        if (quiescence) {
            ++stats.qnodes;
        } else {
            ++stats.nodes;
        }
        return !interrupted();
    }

    [[nodiscard]] int terminal_score(const GameState& state, std::size_t move_count,
                                     int ply) const noexcept {
        if (move_count != 0) {
            return 0;
        }
        return state.in_check() ? -kMateScore + ply : 0;
    }

    [[nodiscard]] int evaluate(const GameState& state, Color perspective) {
        const std::uint64_t key = state.position_key() ^
            (perspective == Color::black ? 0xD6E8FEB86659FD93ULL : 0ULL);
        EvaluationCacheEntry& entry =
            evaluation_cache[static_cast<std::size_t>(key) & (kEvaluationCacheSize - 1)];
        if (entry.valid && entry.key == key) {
            ++stats.evaluation_cache_hits;
            return entry.score;
        }

        int score = 0;
        if (evaluator_mutex != nullptr) {
            std::lock_guard lock(*evaluator_mutex);
            score = evaluator.evaluate(state, perspective);
        } else {
            score = evaluator.evaluate(state, perspective);
        }
        entry = EvaluationCacheEntry{key, score, true};
        return score;
    }

    void record_ply(int ply) noexcept {
        stats.seldepth = std::max(stats.seldepth, ply);
    }

    int quiescence(GameState& state, int alpha, int beta, int ply, int qdepth = 0) {
        if (!count_node(true)) {
            return 0;
        }
        record_ply(ply);

        const bool checked = state.in_check();
        MoveMetadataList moves;
        const bool has_legal_move = checked ?
            (state.legal_moves_with_metadata(moves, true, false), !moves.empty()) :
            state.legal_tactical_moves_with_metadata(
                moves, qdepth < quiescence_check_depth_limit, true);
        if (!has_legal_move) {
            return terminal_score(state, 0, ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }

        // The ordinary tactical generator intentionally stops probing quiet
        // checks after the shallow horizon. At a later qsearch ply, inspect a
        // bounded quiet-check candidate set and retain only checks with a
        // narrow evasion set.
        // This rescues short mating nets without paying the full quiet-check
        // annotation cost at every quiescence node.
        const bool has_checking_capture = std::any_of(
            moves.begin(), moves.end(), [](const MoveMetadata& metadata) {
                return metadata.is_capture() && metadata.gives_check;
            });
        if (!checked && qdepth >= kNarrowQuietCheckProbeStartDepth &&
            qdepth < kMaximumQuiescenceNarrowQuietCheckDepth &&
            has_checking_capture) {
            MoveMetadataList quiet_check_candidates;
            state.legal_moves_with_metadata(
                quiet_check_candidates, true, false, CheckFlagMode::quiet_moves_only);
            for (const MoveMetadata& candidate : quiet_check_candidates) {
                if (candidate.is_capture() || !candidate.gives_check ||
                    !narrow_deep_quiet_check_candidate(state, candidate)) {
                    continue;
                }
                (void)moves.push_back(candidate);
            }
        }

        int best = -kInfinity;
        if (!checked) {
            best = evaluate(state, state.side_to_move());
            if (best >= beta) {
                return best;
            }
            alpha = std::max(alpha, best);
            if (qdepth >= kMaximumQuiescenceDepth) {
                return best;
            }
            if (moves.empty()) {
                return best;
            }
        } else if (qdepth >= kMaximumQuiescenceSafetyDepth) {
            // A checked position has no stand-pat score: the side to move must
            // play an evasion.  The safety limit only prevents pathological
            // perpetual-check trees from exhausting the call stack.
            return 0;
        }

        ordering.order(state, moves, std::nullopt, ply);
        for (const MoveMetadata& metadata : moves) {
            if (interrupted()) {
                return 0;
            }
            if (!checked &&
                qdepth >= quiescence_check_depth_limit - 1 &&
                metadata.gives_check &&
                !metadata.is_capture() &&
                metadata.move.promotion() == Promotion::none &&
                !narrow_deep_quiet_check_candidate(state, metadata)) {
                continue;
            }
            const Move move = metadata.move;
            if (!checked && metadata.is_capture() &&
                move.promotion() == Promotion::none && !metadata.gives_check) {
                if (metadata.see_score < 0) {
                    ++stats.see_prunes;
                    continue;
                }
                const int delta = piece_value(metadata.captured_piece) + 100;
                if (best + delta < alpha) {
                    ++stats.delta_prunes;
                    continue;
                }
            }
            if (metadata.gives_check &&
                (checked || qdepth < quiescence_check_depth_limit)) {
                ++stats.qchecks;
            }
            if (!state.make_search_move(metadata)) {
                continue;
            }
            const int score = -quiescence(state, -beta, -alpha, ply + 1, qdepth + 1);
            state.unmake_move();
            if (aborted) {
                return 0;
            }
            best = std::max(best, score);
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                break;
            }
        }
        return best;
    }

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv, std::optional<Move> previous_move = std::nullopt,
                bool allow_null_pruning = true,
                int check_extensions_remaining = kMaximumCheckExtensionsPerPath) {
        if (ply == 0) {
            root_move_score_count = 0;
        }
        if (depth <= 0) {
            return quiescence(state, alpha, beta, ply);
        }
        if (!count_node(false)) {
            return 0;
        }
        record_ply(ply);

        // Quiescence performs its own terminal-aware tactical/evasion move
        // generation. Do not build and annotate the full legal move list here
        // only to discard it immediately at the depth boundary.
        const bool checked = state.in_check();
        detail::SearchFrame& frame = stack.frame(static_cast<std::size_t>(std::max(ply, 0)));
        frame.previous_move = previous_move.value_or(Move::no_move());
        frame.in_check = checked;
        frame.move_count = 0;
        frame.reduction = 0;
        frame.extension = 0;
        MoveMetadataList moves;
        if (ply == 0 && root_moves != nullptr) {
            moves = *root_moves;
        } else {
            state.legal_moves_with_metadata(
                moves, true, false, CheckFlagMode::quiet_moves_only);
        }
        if (moves.empty()) {
            return terminal_score(state, moves.size(), ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }
        const bool short_timed_root = ply == 0 &&
            time_manager.time_budget().has_value() &&
            *time_manager.time_budget() < kShortTimedFallbackMinimum;
        int child_check_extensions_remaining = check_extensions_remaining;
        if (checked && depth < kMaximumSearchDepth && !short_timed_root &&
            check_extensions_remaining > 0) {
            ++stats.check_extensions;
            ++depth;
            --child_check_extensions_remaining;
            frame.extension = 1;
        }

        std::optional<PositionFeatures> features;
        const auto ensure_features = [&]() -> const PositionFeatures& {
            if (!features.has_value()) {
                const std::uint64_t misses_before = state.position_feature_cache_misses();
                features = state.position_features();
                if (state.position_feature_cache_misses() > misses_before) {
                    ++stats.position_feature_extractions;
                }
            }
            return *features;
        };
        const bool tactical_position = checked || std::any_of(moves.begin(), moves.end(),
            [](const MoveMetadata& metadata) {
                return metadata.is_capture() || metadata.gives_check ||
                    metadata.move.promotion() != Promotion::none;
            });
        int static_eval = 0;
        const bool phase_rich_quiet_position = !checked && depth == 1 && !tactical_position &&
            ensure_features().game_phase >= 8;
        const PositionFeatures* quiet_forcing_parent_features = nullptr;
        const PositionFeatures* root_direct_forcing_features = nullptr;
        if (!checked && depth == 1 && ply == 1) {
            quiet_forcing_parent_features = &ensure_features();
        }
        if (!checked && depth == 1 && ply == 0 && allow_root_forcing_extension) {
            root_direct_forcing_features = &ensure_features();
        }
        if (phase_rich_quiet_position) {
            static_eval = evaluate(state, state.side_to_move());
            frame.static_eval = static_eval;
            if (depth == 1 && alpha > -kInfinity && static_eval + 120 <= alpha) {
                const int razor_score = quiescence(state, alpha, beta, ply);
                if (!aborted && razor_score <= alpha) {
                    ++stats.razoring_prunes;
                    return razor_score;
                }
            }
        }

        const int original_alpha = alpha;
        const int original_beta = beta;
        std::optional<Move> tt_move;
        if (use_transposition_table) {
            if (const auto entry = table.probe(state.position_key(), ply); entry.has_value()) {
                ++stats.tt_hits;
                tt_move = entry->best_move.is_no_move() ? std::nullopt : std::optional<Move>{entry->best_move};
                if (ply > 0 && entry->depth >= depth) {
                    if (entry->bound == TranspositionBound::exact) {
                        return entry->score;
                    }
                    if (entry->bound == TranspositionBound::lower) {
                        alpha = std::max(alpha, entry->score);
                    } else {
                        beta = std::min(beta, entry->score);
                    }
                    if (alpha >= beta) {
                        return entry->score;
                    }
                }
            }
        }

        const bool null_move_candidate = allow_null_pruning && !checked && depth >= 3 &&
            beta < kInfinity && beta > -kInfinity && beta - alpha <= 1;
        if (null_move_candidate && state.is_repetition_sensitive()) {
            ++stats.null_repetition_skips;
        }
        if (null_move_candidate && null_move_is_safe(state, ensure_features())) {
            if (state.make_null_move()) {
                PrincipalVariation null_pv;
                const int reduction = depth >= 6 ? 3 : 2;
                const int null_depth = std::max(0, depth - 1 - reduction);
                const int null_score = -negamax(
                    state, null_depth, -beta, -beta + 1, ply + 1, null_pv,
                    std::nullopt, allow_null_pruning, child_check_extensions_remaining);
                state.unmake_null_move();
                if (aborted) {
                    return 0;
                }
                if (null_score >= beta) {
                    int verified_score = null_score;
                    if (depth >= 5) {
                        ++stats.null_verifications;
                        PrincipalVariation verification_pv;
                        verified_score = negamax(
                            state, depth - 1, alpha, beta, ply, verification_pv,
                            previous_move, false, child_check_extensions_remaining);
                        if (aborted) {
                            return 0;
                        }
                    }
                    if (verified_score >= beta) {
                        ++stats.null_cutoffs;
                        if (use_transposition_table) {
                            table.store(state.position_key(), depth, verified_score,
                                        TranspositionBound::lower, Move::no_move(), ply);
                        }
                        return verified_score;
                    }
                }
            }
        }

        ordering.order(state, moves, tt_move, ply, previous_move);
        int best_score = -kInfinity;
        Move best_move = Move::no_move();
        int move_number = 0;
        std::optional<PositionFeatures> lmr_parent_features;
        for (const MoveMetadata& metadata : moves) {
            if (interrupted()) {
                return 0;
            }
            const Move move = metadata.move;
            frame.current_move = move;
            const Color moving_side = history_side(state.side_to_move(), false);
            const int history_score = ordering.quiet_history_score(moving_side, move, previous_move);
            const bool is_tt_move = tt_move.has_value() && move == *tt_move;
            const bool root_pawn_move = ply == 0 && metadata.moving_piece == PieceType::pawn;
            const bool lmr_base_candidate = !root_pawn_move && move_number >= 4 && depth >= 4 && !checked &&
                !metadata.gives_check && !metadata.is_capture() &&
                move.promotion() == Promotion::none && !is_tt_move &&
                !ordering.is_killer(move, ply);
            const bool high_history_exclusion = lmr_base_candidate &&
                detail::high_history_move_excluded_from_lmr(history_score);
            if (high_history_exclusion) {
                ++stats.lmr_high_history_exclusions;
            }
            const bool lmr_candidate = lmr_base_candidate && !high_history_exclusion;
            if (lmr_candidate) {
                if (!lmr_parent_features.has_value()) {
                    const std::uint64_t misses_before = state.position_feature_cache_misses();
                    lmr_parent_features = state.position_features();
                    if (state.position_feature_cache_misses() > misses_before) {
                        ++stats.position_feature_extractions;
                    }
                } else {
                    ++stats.lmr_parent_feature_reuses;
                }
            }
            if (!state.make_search_move(metadata)) {
                continue;
            }

            bool king_zone_pressure = false;
            bool reducible_quiet = false;
            if (lmr_candidate && lmr_parent_features.has_value()) {
                ++stats.position_feature_extractions;
                const PositionFeatures after_quiet_features = state.position_features();
                const std::size_t enemy = lmr_parent_features->side_to_move == Color::white ? 1U : 0U;
                king_zone_pressure = after_quiet_features.king_zone_attacks[enemy] >
                    lmr_parent_features->king_zone_attacks[enemy];
                reducible_quiet = !state.in_check() &&
                    !quiet_move_is_forcing(*lmr_parent_features, after_quiet_features, metadata);
            }
            if (king_zone_pressure) {
                ++stats.lmr_king_zone_exclusions;
            }
            bool quiet_forcing_extension = false;
            if ((quiet_forcing_parent_features != nullptr || root_direct_forcing_features != nullptr) &&
                !metadata.is_capture() && !metadata.gives_check &&
                metadata.move.promotion() == Promotion::none) {
                const PositionFeatures& forcing_parent_features =
                    root_direct_forcing_features != nullptr ? *root_direct_forcing_features :
                    *quiet_forcing_parent_features;
                const bool direct_forcing_target =
                    quiet_move_has_direct_forcing_target(forcing_parent_features, metadata);
                const bool near_root_forcing_target = quiet_forcing_parent_features != nullptr &&
                    (quiet_move_has_king_ring_target(forcing_parent_features, metadata) ||
                     quiet_move_has_pawn_break_target(metadata));
                if (direct_forcing_target || near_root_forcing_target) {
                    const std::uint64_t misses_before = state.position_feature_cache_misses();
                    const PositionFeatures after_quiet_features = state.position_features();
                    if (state.position_feature_cache_misses() > misses_before) {
                        ++stats.position_feature_extractions;
                    }
                    quiet_forcing_extension = quiet_move_is_forcing(
                        forcing_parent_features, after_quiet_features, metadata);
                    if (quiet_forcing_extension) {
                        ++stats.quiet_forcing_extensions;
                    }
                }
            }
            const int full_child_depth = depth - 1;
            const int base_reduction = 1 + (depth >= 8 && move_number >= 12 ? 1 : 0) +
                (depth >= 12 && move_number >= 20 ? 1 : 0);
            const int history_adjustment = history_score > 256 ? -1 : history_score < -256 ? 1 : 0;
            const int reduction = std::clamp(base_reduction + history_adjustment, 0, full_child_depth);
            const bool reduced = reducible_quiet && !quiet_forcing_extension && reduction > 0;
            frame.reduction = reduced ? reduction : 0;
            const int authoritative_child_depth = quiet_forcing_extension ?
                full_child_depth + 1 : full_child_depth;
            const int child_depth = reduced ? full_child_depth - reduction : authoritative_child_depth;
            if (reduced) {
                ++stats.lmr_reductions;
            }

            if (phase_rich_quiet_position && !metadata.is_capture() &&
                !metadata.gives_check && move.promotion() == Promotion::none && move_number > 0 &&
                static_eval + 80 + depth * 60 <= alpha) {
                ++stats.quiet_futility_prunes;
                state.unmake_move();
                ++move_number;
                continue;
            }

            PrincipalVariation child_pv;
            int score = 0;
            const bool root_forcing_move = allow_root_forcing_extension &&
                ply == 0 && quiet_forcing_extension;
            if (move_number == 0) {
                score = -negamax(
                    state, child_depth, -beta, -alpha, ply + 1, child_pv,
                    move, allow_null_pruning, child_check_extensions_remaining);
            } else if (root_forcing_move) {
                score = -negamax(
                    state, child_depth, -kInfinity, kInfinity, ply + 1, child_pv,
                    move, allow_null_pruning, child_check_extensions_remaining);
            } else {
                ++stats.pvs_searches;
                score = -negamax(
                    state, child_depth, -alpha - 1, -alpha, ply + 1, child_pv,
                    move, allow_null_pruning, child_check_extensions_remaining);
                if (!aborted && reduced && score > alpha) {
                    ++stats.lmr_verifications;
                    child_pv = {};
                    score = -negamax(
                        state, full_child_depth, -beta, -alpha, ply + 1, child_pv,
                        move, allow_null_pruning, child_check_extensions_remaining);
                } else if (!aborted && !reduced && score > alpha && score < beta) {
                    ++stats.pvs_researches;
                    child_pv = {};
                    score = -negamax(
                        state, authoritative_child_depth, -beta, -alpha, ply + 1, child_pv,
                        move, allow_null_pruning, child_check_extensions_remaining);
                }
            }
            state.unmake_move();
            if (aborted) {
                return 0;
            }

            if (score > best_score) {
                best_score = score;
                best_move = move;
                pv.prepend(move, child_pv);
            }
            if (ply == 0 && root_move_score_count < root_move_scores.size()) {
                root_move_scores[root_move_score_count++] = RootMoveScore{move, score};
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (ply > 0 && !metadata.is_capture() && move.promotion() == Promotion::none) {
                    const Color history_side_after_unmake = history_side(moving_side, true);
                    ordering.record_quiet_cutoff(history_side_after_unmake, move, ply, depth,
                                                 previous_move);
                    ++stats.quiet_history_updates;
                    if (previous_move.has_value()) {
                        ++stats.continuation_history_updates;
                    }
                }
                break;
            }
            if (ply > 0 && !metadata.is_capture() && move.promotion() == Promotion::none) {
                const Color history_side_after_unmake = history_side(moving_side, true);
                ordering.record_quiet_fail(history_side_after_unmake, move, ply, depth,
                                           previous_move);
                ++stats.quiet_history_updates;
                if (previous_move.has_value()) {
                    ++stats.continuation_history_updates;
                }
            }
            ++move_number;
            frame.move_count = move_number;
        }

        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= original_beta ? TranspositionBound::lower : TranspositionBound::exact;
        if (use_transposition_table) {
            table.store(state.position_key(), depth, best_score, bound, best_move, ply);
        }
        return best_score;
    }
};

class RootWorkerPool {
public:
    RootWorkerPool(std::size_t worker_count, const Evaluator& evaluator, TranspositionTable& table,
                   TimeManager& time_manager, std::atomic_bool& stop_requested,
                   std::atomic<std::uint64_t>* global_nodes, std::mutex* evaluator_mutex,
                   bool use_transposition_table,
                   SearchOptions::QuietHistorySideHook quiet_history_side_hook = {})
        : worker_count_(std::max<std::size_t>(1, worker_count)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested), global_nodes_(global_nodes),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          quiet_history_side_hook_(std::move(quiet_history_side_hook)), worker_stats_(worker_count_) {
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
             std::vector<RootLine>& lines, bool use_root_pvs,
             SearchStats& stats, bool& aborted) {
        auto job = std::make_shared<Job>();
        job->root = &root;
        job->root_moves = &root_moves;
        job->depth = depth;
        job->root_check_extension = root_check_extension;
        job->stable_root_indices = &stable_root_indices;
        job->lines = &lines;
        job->worker_stats = &worker_stats_;
        job->use_root_pvs = use_root_pvs;
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
        std::atomic<std::size_t> next_move = 0;
        std::atomic<int> root_alpha = -kInfinity;
        std::atomic_bool aborted = false;
        bool use_root_pvs = false;
        std::uint64_t sequence = 0;
    };

    void worker_loop(std::size_t worker_index) {
        // Root jobs are authoritative. The striped table keeps concurrent probe/store activity
        // safe without requiring a serial confirmation search.
        SearchContext context(evaluator_, table_, time_manager_, stop_requested_, global_nodes_,
                              evaluator_mutex_, nullptr, use_transposition_table_,
                              quiet_history_side_hook_);
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

            const auto search_root = [&context, &job](std::size_t move_index) {
                const MoveMetadata& root_move = (*job->root_moves)[move_index];
                try {
                    context.ordering.clear();
                    GameState child = *job->root;
                    if (!child.make_search_move(root_move)) {
                        return;
                    }

                    PrincipalVariation child_pv;
                    int child_depth = job->depth - 1 +
                        (job->root_check_extension ? 1 : 0);
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
                    const bool scout = job->use_root_pvs && shared_alpha > -kInfinity;
                    int score = 0;
                    if (scout) {
                        ++context.stats.root_pvs_searches;
                        score = -context.negamax(child, child_depth,
                                                 -shared_alpha - 1, -shared_alpha, 1, child_pv,
                                                 root_move.move);
                        if (!context.aborted && score > shared_alpha) {
                            ++context.stats.root_pvs_researches;
                            child_pv = {};
                            score = -context.negamax(child, child_depth,
                                                     -kInfinity, kInfinity, 1, child_pv,
                                                     root_move.move);
                        }
                    } else {
                        score = -context.negamax(child, child_depth,
                                                 -kInfinity, kInfinity, 1, child_pv,
                                                 root_move.move);
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
                    while (score > observed_alpha &&
                           !job->root_alpha.compare_exchange_weak(
                               observed_alpha, score, std::memory_order_relaxed,
                               std::memory_order_relaxed)) {
                    }

                    RootLine& line = (*job->lines)[move_index];
                    line.score = score;
                    line.stable_index = (*job->stable_root_indices)[move_index];
                    line.pv.prepend(root_move.move, child_pv);
                    line.completed = true;
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

struct SearchService::Impl {
    explicit Impl(std::shared_ptr<const Evaluator> evaluator, HashMemoryPolicy hash_memory_policy,
                  std::size_t initial_hash_mb)
        : evaluator(std::move(evaluator)),
          table(std::make_shared<TranspositionTable>(initial_hash_mb, std::move(hash_memory_policy))),
          evaluator_mutex(std::make_shared<std::mutex>()) {}

    std::shared_ptr<const Evaluator> evaluator;
    std::shared_ptr<TranspositionTable> table;
    std::shared_ptr<std::mutex> evaluator_mutex;
};

SearchHandle::SearchHandle(std::shared_ptr<detail::SearchSession> state) noexcept
    : state_(std::move(state)) {}

SearchHandle::SearchHandle(SearchHandle&&) noexcept = default;

SearchHandle& SearchHandle::operator=(SearchHandle&& other) noexcept {
    if (this != &other) {
        stop();
        wait();
        state_ = std::move(other.state_);
    }
    return *this;
}

SearchHandle::~SearchHandle() {
    stop();
    wait();
}

void SearchHandle::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

void SearchHandle::wait() {
    if (state_) {
        state_->wait();
    }
}

bool SearchHandle::running() const noexcept {
    return state_ && state_->running();
}

SearchService::SearchService(std::shared_ptr<const Evaluator> evaluator,
                             HashMemoryPolicy hash_memory_policy,
                             std::size_t initial_hash_mb) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires an evaluator");
    }
    impl_ = std::make_shared<Impl>(std::move(evaluator), std::move(hash_memory_policy),
                                   initial_hash_mb);
}

SearchHandle SearchService::start(GameState root, SearchLimits limits, SearchEventSink sink,
                                  SearchOptions options) {
    // The default SearchOptions hash value means "use the service configuration".
    // A non-default value explicitly reconfigures the shared service table.
    if (options.hash_mb != SearchOptions{}.hash_mb) {
        (void)impl_->table->set_size_mb(options.hash_mb);
    }

    options.threads = normalized_threads(options.threads);
    options.speed_percent = normalized_speed(options.speed_percent);
    options.multi_pv = normalized_multi_pv(options.multi_pv);
    options.move_overhead_ms = normalized_move_overhead(options.move_overhead_ms);
    options.slow_mover_percent = normalized_slow_mover(options.slow_mover_percent);
    options.elo = normalized_elo(options.elo);
    if (options.limit_strength && options.strength_profile_hook) {
        options.strength_profile_hook(options);
    }

    auto session = std::make_shared<detail::SearchSession>(
        std::move(root), std::move(limits), options);
    const auto evaluator = impl_->evaluator;
    const auto table = impl_->table;
    const auto evaluator_mutex = impl_->evaluator_mutex;
    session->launch([session, evaluator, table, sink = std::move(sink), evaluator_mutex]() mutable {
        const auto started = std::chrono::steady_clock::now();
        GameState root = session->root();
        const SearchLimits& limits = session->limits();
        const SearchOptions& options = session->options();
        SearchResult result;
        result.identity = session->identity();
        try {
            RootTimingContext timing_context;
            timing_context.table_available = table->size_mb() != 0;
            if (const auto entry = table->probe(root.position_key(), 0); entry.has_value()) {
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
            table->new_generation();
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
            const bool root_is_claimable_draw = root.is_draw_by_rule();

            std::optional<SyzygyRootResult> tablebase_result;
            if (options.syzygy && options.multi_pv == 1 && !options.analyse_mode &&
                !limits.ponder && !limits.search_moves_specified && !root_is_claimable_draw &&
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
                safely_report_info(sink, info);
            } else if ((options.threads == 1 && options.multi_pv == 1) || legal_moves.size() < 2 ||
                       (time_manager.node_limit().has_value() && options.multi_pv == 1) ||
                       (short_tactical_budget && !ultra_short_nonchecking_parallel)) {
                SearchContext context(*evaluator, *table, time_manager, session->stop_requested(),
                                      nullptr, evaluator_mutex_ptr, &legal_moves, true,
                                      options.quiet_history_side_hook);
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
                        result.best_move = *fallback;
                        result.pv = {*fallback};
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

                const int maximum_depth =
                    std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                const bool unbounded = limits.infinite || limits.ponder;
                std::optional<int> previous_score;
                std::chrono::milliseconds previous_iteration_elapsed{1};
                std::chrono::milliseconds previous_report_elapsed{0};
                for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
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
                    PrincipalVariation pv;
                    int alpha = -kInfinity;
                    int beta = kInfinity;
                    bool aspiration_researched = false;
                    if (previous_score.has_value()) {
                        alpha = std::max(-kInfinity, *previous_score - kAspirationWindow);
                        beta = std::min(kInfinity, *previous_score + kAspirationWindow);
                    }

                    int score = context.negamax(root, depth, alpha, beta, 0, pv);
                    if (!context.aborted && previous_score.has_value() &&
                        (score <= alpha || score >= beta)) {
                        ++context.stats.aspiration_researches;
                        aspiration_researched = true;
                        pv = {};
                        score = context.negamax(root, depth, -kInfinity, kInfinity, 0, pv);
                    }
                    if (context.aborted) {
                        if (result.completed_depth == 0 && !used_short_fallback &&
                            defer_nonchecking_short_fallback) {
                            if (const auto fallback = short_search_fallback_move(
                                    root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                    &context.stats, allow_deep_checked_fallback, &fallback_control,
                                    true, true);
                                fallback.has_value()) {
                                result.best_move = *fallback;
                                result.pv = {*fallback};
                                used_short_fallback = true;
                            }
                        }
                        if (result.completed_depth == 0 && !used_short_fallback &&
                            ultra_short_checked_fallback) {
                            if (const auto fallback = short_search_fallback_move(
                                    root, legal_moves, *evaluator, evaluator_mutex_ptr,
                                    &context.stats, false, &fallback_control, true);
                                fallback.has_value()) {
                                result.best_move = *fallback;
                                result.pv = {*fallback};
                                used_short_fallback = true;
                            }
                        }
                        if (result.completed_depth == 0 && !used_short_fallback) {
                            // An interrupted first iteration is never an
                            // authoritative tactical result.  Reject a
                            // partial root move that exposes an immediate
                            // checking reply even when the root move list did
                            // not contain a capture or direct check.
                            if (const auto partial = context.best_completed_root_move(
                                    root, true);
                                partial.has_value() && root.is_legal(partial->move)) {
                                result.best_move = partial->move;
                                result.pv = {partial->move};
                                result.score_cp = partial->score;
                            }
                        }
                        break;
                    }

                    result.completed_depth = depth;
                    result.score_cp = score;
                    result.mate = mate_from_score(score);
                    if (pv.length > 0) {
                        result.best_move = pv.moves[0];
                        result.pv = pv.to_vector();
                    }

                    if ((short_tactical_fallback || defer_nonchecking_short_fallback) &&
                        !defer_expensive_short_fallback &&
                        result.completed_depth == 1 &&
                        (short_capture_needs_fallback(legal_moves, result.best_move) ||
                         short_quiet_needs_fallback(legal_moves, result.best_move) ||
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
                        const auto current_metadata = result.best_move.has_value() ?
                            std::find_if(
                                legal_moves.begin(), legal_moves.end(),
                                [&result](const MoveMetadata& metadata) {
                                    return metadata.move == *result.best_move;
                                }) : legal_moves.end();
                        if (current_metadata != legal_moves.end() &&
                            !current_metadata->is_capture() && !current_metadata->gives_check &&
                            current_metadata->move.promotion() == Promotion::none &&
                            (!fallback.has_value() || *fallback == current_metadata->move)) {
                            fallback = first_safe_checking_capture(legal_moves);
                        }
                        if (fallback.has_value() &&
                            short_fallback_should_replace_completed_root(
                                root, legal_moves, result.best_move, *fallback)) {
                            result.best_move = *fallback;
                            result.pv = {*fallback};
                            // Keep the protocol PV in lockstep with the
                            // corrected authoritative root move. The normal
                            // iteration PV still contains the pre-correction
                            // move, and publishing it here would make the
                            // following bestmove disagree with the final
                            // info line.
                            pv = {};
                            pv.moves[0] = *fallback;
                            pv.length = 1;
                        }
                    }

                    // Selective root checks below may run another negamax call,
                    // which refreshes the context's root-score scratch buffer.
                    // Preserve the completed full-width scores before any such
                    // check so later safety decisions remain based on this
                    // iteration's complete root ordering.
                    std::vector<SearchContext::RootMoveScore> root_score_snapshot;
                    if (limits.depth.has_value() && *limits.depth == kRootSelectiveDepth &&
                        depth == kRootSelectiveDepth && context.root_move_score_count != 0) {
                        root_score_snapshot.assign(
                            context.root_move_scores.begin(),
                            context.root_move_scores.begin() + context.root_move_score_count);
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
                                MoveMetadata metadata;
                            };
                            std::array<QuietCandidate, 2> quiet_candidates{};
                            std::size_t quiet_count = 0;
                            for (std::size_t index = 0; index < context.root_move_score_count; ++index) {
                                const SearchContext::RootMoveScore& root_score =
                                    context.root_move_scores[index];
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
                                if (extended_move.has_value()) {
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
                        } else if (best_metadata != legal_moves.end() &&
                                   !best_metadata->is_capture() && !best_metadata->gives_check &&
                                   best_metadata->move.promotion() == Promotion::none) {
                            // The inverse horizon trap is a quiet move that is only a few
                            // centipawns ahead of a check, capture, or promotion.  At an
                            // incomplete depth-one root the quiet score is not authoritative;
                            // compare the quiet move and a small forcing band at depth two.
                            std::array<MoveMetadata, 4> candidates{};
                            std::size_t candidate_count = 0;
                            candidates[candidate_count++] = *best_metadata;
                            for (std::size_t index = 0;
                                 index < context.root_move_score_count && candidate_count < candidates.size();
                                 ++index) {
                                const SearchContext::RootMoveScore& root_score =
                                    context.root_move_scores[index];
                                if (root_score.move == *result.best_move ||
                                    root_score.score < score - shallow_root_margin) {
                                    continue;
                                }
                                const auto metadata = std::find_if(
                                    legal_moves.begin(), legal_moves.end(),
                                    [&root_score](const MoveMetadata& move_metadata) {
                                        return move_metadata.move == root_score.move;
                                    });
                                if (metadata == legal_moves.end() ||
                                    (!metadata->is_capture() && !metadata->gives_check &&
                                     metadata->move.promotion() == Promotion::none)) {
                                    continue;
                                }
                                candidates[candidate_count++] = *metadata;
                            }

                            if (candidate_count > 1) {
                                int extended_score = -kInfinity;
                                std::optional<Move> extended_move;
                                bool extended_forcing = false;
                                PrincipalVariation extended_pv;
                                for (std::size_t index = 0; index < candidate_count; ++index) {
                                    ++context.stats.root_selective_candidates;
                                    const SearchContext::RootVerification verification =
                                        context.selectively_research_root_move(root, candidates[index],
                                                                               depth + 1);
                                    if (context.aborted) {
                                        break;
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
                                if (extended_move.has_value()) {
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
                                if (verification.score > shallow_score + kRootSelectiveImprovement) {
                                    ++context.stats.root_selective_researches;
                                    score = verification.score;
                                    result.score_cp = score;
                                    result.mate = mate_from_score(score);
                                    result.best_move = candidate.move;
                                    pv = verification.pv;
                                    result.pv = pv.to_vector();
                                    break;
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
                                if (!context.aborted) {
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
                                        if (verification.score > selected_score ||
                                            verification.score >= best_verification.score -
                                                kRootKingSafetyTieMargin) {
                                            selected_score = verification.score;
                                            selected_move = candidate.move;
                                            selected_pv = verification.pv;
                                        }
                                    }
                                    if (!context.aborted) {
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
                    safely_report_info(sink, info);
                    time_manager.observe_iteration(SearchIterationObservation{
                        depth,
                        score,
                        had_completed_iteration && previous_best_move != result.best_move,
                        had_completed_iteration && previous_pv != result.pv,
                        aspiration_researched,
                        visited});
                    result.timing = time_manager.diagnostics();
                    previous_score = score;
                    previous_iteration_elapsed = elapsed - previous_report_elapsed;
                    previous_report_elapsed = elapsed;

                    if ((!unbounded && depth == maximum_depth) || root_is_claimable_draw ||
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
                                    options.quiet_history_side_hook);
                SearchStats total_stats;
                SearchContext root_context(*evaluator, *table, time_manager, session->stop_requested(),
                                           global_nodes_ptr, evaluator_mutex_ptr, nullptr, true,
                                           options.quiet_history_side_hook);

                MoveMetadataList parallel_moves = legal_moves;
                detail::SearchMoveOrdering root_ordering;
                root_ordering.order(root, parallel_moves, std::nullopt, 0);
                bool used_short_fallback = false;
                const bool defer_nonchecking_short_fallback =
                    ultra_short_nonchecked_fallback || ultra_short_nonchecked_forcing_root;
                if (short_tactical_fallback && !defer_nonchecking_short_fallback &&
                    !defer_expensive_short_fallback) {
                    if (const auto fallback = short_search_fallback_move(
                            root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                            &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                        fallback.has_value()) {
                        result.best_move = *fallback;
                        result.pv = {*fallback};
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
                const int maximum_depth =
                    std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
                const bool unbounded = limits.infinite || limits.ponder;
                std::chrono::milliseconds previous_iteration_elapsed{1};
                std::chrono::milliseconds previous_report_elapsed{0};
                for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
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
                    const bool root_check_extension = root.in_check() && depth < kMaximumSearchDepth;
                    if (root_check_extension) {
                        ++root_context.stats.check_extensions;
                    }

                    std::optional<Move> tt_move;
                    if (const auto entry = table->probe(root.position_key(), 0); entry.has_value()) {
                        ++root_context.stats.tt_hits;
                        if (!entry->best_move.is_no_move()) {
                            tt_move = entry->best_move;
                        }
                    }
                    root_ordering.order(root, parallel_moves, tt_move, 0);
                    std::vector<std::size_t> stable_root_indices;
                    stable_root_indices.reserve(parallel_moves.size());
                    for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                        stable_root_indices.push_back(index);
                    }

                    std::vector<RootLine> lines(parallel_moves.size());

                    SearchStats iteration_stats;
                    bool aborted = false;
                    pool.run(root, parallel_moves, depth, root_check_extension, stable_root_indices,
                             lines, !multi_pv, iteration_stats, aborted);
                    if (aborted) {
                        if (result.completed_depth == 0 && !used_short_fallback &&
                            defer_nonchecking_short_fallback) {
                            if (const auto fallback = short_search_fallback_move(
                                    root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                    &root_context.stats, allow_deep_checked_fallback, &fallback_control,
                                    true, true);
                                fallback.has_value()) {
                                result.best_move = *fallback;
                                result.pv = {*fallback};
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
                            [&parallel_moves, &root](const std::size_t index) {
                                return index < parallel_moves.size() &&
                                    !root_move_exposes_immediate_check(root, parallel_moves[index]);
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

                    std::vector<std::size_t> ranked_indices;
                    if (multi_pv) {
                        ranked_indices = RootCoordinator::rank(lines);
                    } else {
                        ranked_indices.reserve(lines.size());
                        for (std::size_t index = 0; index < lines.size(); ++index) {
                            if (lines[index].completed) {
                                ranked_indices.push_back(index);
                            }
                        }
                        std::stable_sort(ranked_indices.begin(), ranked_indices.end(),
                                         [&lines](std::size_t left, std::size_t right) {
                                             return lines[left].score > lines[right].score;
                                         });
                    }
                    if (ranked_indices.empty()) {
                        if (result.completed_depth == 0 && !used_short_fallback &&
                            defer_nonchecking_short_fallback) {
                            if (const auto fallback = short_search_fallback_move(
                                    root, parallel_moves, *evaluator, evaluator_mutex_ptr,
                                    &root_context.stats, allow_deep_checked_fallback, &fallback_control);
                                fallback.has_value()) {
                                result.best_move = *fallback;
                                result.pv = {*fallback};
                                used_short_fallback = true;
                            }
                        }
                        accumulate_stats(iteration_stats, root_context.stats);
                        accumulate_stats(total_stats, iteration_stats);
                        break;
                    }

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
                                if (verification.score > shallow_best.score +
                                        kRootSelectiveImprovement) {
                                    ++root_context.stats.root_selective_researches;
                                    lines[candidate_index].score = verification.score;
                                    lines[candidate_index].pv = verification.pv;
                                    lines[candidate_index].completed = true;
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

                            for (const auto& [candidate_index, metadata] : candidates) {
                                ++root_context.stats.root_selective_candidates;
                                const SearchContext::RootVerification verification =
                                    root_context.selectively_research_root_move(root, metadata, depth + 1);
                                if (root_context.aborted) {
                                    break;
                                }
                                const bool candidate_forcing = metadata.is_capture() || metadata.gives_check ||
                                    metadata.move.promotion() != Promotion::none;
                                if (verification.score > shallow_best.score + kRootSelectiveImprovement ||
                                    (candidate_forcing &&
                                     verification.score >= shallow_best.score - kRootSelectiveMargin)) {
                                    ++root_context.stats.root_selective_researches;
                                    lines[candidate_index].score = verification.score;
                                    lines[candidate_index].pv = verification.pv;
                                    lines[candidate_index].completed = true;
                                    ranked_indices = RootCoordinator::rank(lines);
                                    break;
                                }
                            }
                        }
                    }

                    accumulate_stats(iteration_stats, root_context.stats);
                    accumulate_stats(total_stats, iteration_stats);
                    if (root_context.aborted) {
                        break;
                    }

                    const std::optional<Move> previous_best_move = result.best_move;
                    const std::vector<Move> previous_pv = result.pv;
                    const bool had_completed_iteration = result.completed_depth > 0;
                    const std::size_t best_index = ranked_indices.front();
                    const RootLine& best_line = lines[best_index];

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

                    if ((short_tactical_fallback || defer_nonchecking_short_fallback) &&
                        !defer_expensive_short_fallback &&
                        result.completed_depth == 1 &&
                        (short_capture_needs_fallback(parallel_moves, result.best_move) ||
                         short_quiet_needs_fallback(parallel_moves, result.best_move) ||
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
                        const auto current_metadata = result.best_move.has_value() ?
                            std::find_if(
                                parallel_moves.begin(), parallel_moves.end(),
                                [&result](const MoveMetadata& metadata) {
                                    return metadata.move == *result.best_move;
                                }) : parallel_moves.end();
                        if (current_metadata != parallel_moves.end() &&
                            !current_metadata->is_capture() && !current_metadata->gives_check &&
                            current_metadata->move.promotion() == Promotion::none &&
                            (!fallback.has_value() || *fallback == current_metadata->move)) {
                            fallback = first_safe_checking_capture(parallel_moves);
                        }
                        const bool should_replace = fallback.has_value() &&
                            short_fallback_should_replace_completed_root(
                                root, parallel_moves, result.best_move, *fallback);
                        if (should_replace) {
                            result.best_move = *fallback;
                            result.pv = {*fallback};
                            // The info loop below reads the ranked root line,
                            // not result.pv. Replace that line as well so a
                            // bounded safety correction cannot publish a PV
                            // for the superseded move before bestmove.
                            if (!ranked_indices.empty()) {
                                RootLine& corrected_line = lines[ranked_indices.front()];
                                corrected_line.pv = {};
                                corrected_line.pv.moves[0] = *fallback;
                                corrected_line.pv.length = 1;
                            }
                        }
                    }

                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
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
                        info.multipv = static_cast<int>(rank + 1);
                        safely_report_info(sink, info);
                    }
                    const auto iteration_elapsed = elapsed - previous_report_elapsed;
                    time_manager.observe_iteration(SearchIterationObservation{
                        depth,
                        best_line.score,
                        had_completed_iteration && previous_best_move != result.best_move,
                        had_completed_iteration && previous_pv != result.pv,
                        false,
                        visited});
                    result.timing = time_manager.diagnostics();
                    previous_iteration_elapsed = iteration_elapsed;
                    previous_report_elapsed = elapsed;
                    if ((!unbounded && depth == maximum_depth) || root_is_claimable_draw ||
                        time_manager.should_stop_after_iteration()) {
                        break;
                    }
                }
                result.stats = total_stats;
            }

            if (limits.ponder && (legal_moves.empty() || root_is_claimable_draw)) {
                session->wait_until_stopped();
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
    });
    return SearchHandle(std::move(session));
}

HashResizeResult SearchService::set_hash_size_mb(std::size_t megabytes) {
    return impl_->table->set_size_mb(megabytes);
}

std::size_t SearchService::hash_size_mb() const noexcept {
    return impl_->table->size_mb();
}

void SearchService::clear_hash() noexcept {
    impl_->table->clear();
}

} // namespace koi
