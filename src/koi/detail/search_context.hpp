#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "koi/detail/search_constants.hpp"
#include "koi/detail/search_context_support.hpp"
#include "koi/detail/search_ordering.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/evaluator.hpp"
#include "koi/search_types.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi::detail {

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

    static constexpr std::size_t stack_capacity() noexcept {
        return SearchStack::kCapacity;
    }

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
    SearchMoveOrdering ordering;
    SearchStats stats;
    bool aborted = false;
    bool allow_root_forcing_extension = false;
    int quiescence_check_depth_limit = kMaximumQuiescenceCheckDepth;
    SearchStack stack;
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

    ~SearchContext();

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
        SearchFrame& frame = stack.frame(static_cast<std::size_t>(std::max(ply, 0)));
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
                high_history_move_excluded_from_lmr(history_score);
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

} // namespace koi::detail
