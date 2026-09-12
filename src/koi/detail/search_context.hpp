#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "koi/detail/search_constants.hpp"
#include "koi/detail/search_context_support.hpp"
#include "koi/detail/evaluation_context.hpp"
#include "koi/detail/search_ordering.hpp"
#include "koi/detail/search_policy.hpp"
#include "koi/detail/search_budget.hpp"
#include "koi/detail/search_stack.hpp"
#include "koi/detail/search_table_access.hpp"
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

    static constexpr bool has_evaluation_context() noexcept {
        return true;
    }

    EvaluationContext evaluation;
    SearchTableAccess table_access;
    SearchBudget budget;
    TimeManager& time_manager;
    std::atomic_bool& stop_requested;
    const MoveMetadataList* root_moves = nullptr;
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
        : evaluation(evaluator, evaluator_mutex), table_access(table, use_transposition_table),
          budget(time_manager, global_nodes), time_manager(time_manager),
          stop_requested(stop_requested), root_moves(root_moves),
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
        const bool saved_use_transposition_table = table_access.enabled();
        root_moves = &single_move;
        // The first shallow root pass stores exact child entries at the same
        // depth that this verification is meant to extend.  Reusing those
        // entries would make the "deeper" comparison identical to the
        // shallow score and hide the tactical consequence being tested.
        table_access.set_enabled(false);
        RootVerification verification;
        try {
            verification.score = negamax(root, depth, -kInfinity, kInfinity, 0,
                                         verification.pv);
        } catch (...) {
            root_moves = saved_root_moves;
            table_access.set_enabled(saved_use_transposition_table);
            throw;
        }
        root_moves = saved_root_moves;
        table_access.set_enabled(saved_use_transposition_table);
        return verification;
    }

    void request_abort() noexcept {
        aborted = true;
        if (iteration_aborted != nullptr) {
            iteration_aborted->store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t visited_nodes() const noexcept {
        return budget.visited(stats.nodes + stats.qnodes);
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
        if (!budget.reserve(stats.nodes + stats.qnodes)) {
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

        const int score = evaluation.evaluate(state, perspective);
        entry = EvaluationCacheEntry{key, score, true};
        return score;
    }

    void record_ply(int ply) noexcept {
        stats.seldepth = std::max(stats.seldepth, ply);
    }

    [[nodiscard]] SearchHistoryContext history_context(
        const int ply, const std::optional<Move> previous_move,
        const std::uint64_t pawn_key) const noexcept {
        SearchHistoryContext context;
        context.ply = ply;
        context.pawn_key = pawn_key;

        Move candidate = previous_move.value_or(Move::no_move());
        if (!previous_move.has_value() && ply > 0) {
            candidate = stack.frame(static_cast<std::size_t>(ply)).previous_move;
        }
        for (std::size_t distance = 0; distance < kSearchContinuationPlies &&
             !candidate.is_no_move(); ++distance) {
            context.continuation_moves[distance] = candidate;
            ++context.count;
            const int parent_ply = ply - static_cast<int>(distance) - 1;
            if (parent_ply < 0) {
                break;
            }
            candidate = stack.frame(static_cast<std::size_t>(parent_ply)).previous_move;
        }
        return context;
    }

    int quiescence(GameState& state, int alpha, int beta, int ply, int qdepth = 0,
                   std::optional<Move> previous_move = std::nullopt) {
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

        const SearchHistoryContext history = history_context(ply, previous_move,
                                                               state.position_key());
        std::optional<Move> tt_move;
        if (const auto entry = table_access.probe(state.position_key(), ply); entry.has_value()) {
            ++stats.tt_hits;
            if (!entry->best_move.is_no_move()) {
                tt_move = entry->best_move;
            }
            // Regular-search TT entries do not carry a qsearch depth. Keep
            // their move as an ordering hint, but do not reuse their bound as
            // a qsearch score: a shallow full-width entry can contain a
            // horizon-dependent result that is not exact at this frontier.
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

        ordering.order(state, moves, tt_move, history);
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
            const int capture_history = ordering.capture_history_score(metadata);
            const int adjusted_see = metadata.is_capture() ?
                static_cast<int>(metadata.see_score) + capture_history / 128 :
                static_cast<int>(metadata.see_score);
            const auto capture_prune = SearchPolicy::quiescence_capture(
                checked, metadata.is_capture(), metadata.gives_check,
                move.promotion() != Promotion::none, adjusted_see,
                piece_value(metadata.captured_piece), best, alpha);
            if (capture_prune == QuiescenceCapturePrune::static_exchange) {
                    ++stats.see_prunes;
                    continue;
            }
            if (capture_prune == QuiescenceCapturePrune::delta) {
                ++stats.delta_prunes;
                continue;
            }
            if (metadata.gives_check &&
                (checked || qdepth < quiescence_check_depth_limit)) {
                ++stats.qchecks;
            }
            if (!state.make_search_move(metadata)) {
                continue;
            }
            const int score = -quiescence(state, -beta, -alpha, ply + 1, qdepth + 1,
                                          metadata.move);
            state.unmake_move();
            if (aborted) {
                return 0;
            }
            if (metadata.is_capture() && score > best) {
                ordering.record_capture_best(metadata, 1, history);
            }
            best = std::max(best, score);
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (metadata.is_capture()) {
                    ordering.record_capture_cutoff(metadata, 1, history);
                }
                break;
            }
            if (metadata.is_capture()) {
                ordering.record_capture_fail(metadata, 1, history);
            }
        }
        return best;
    }

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv, std::optional<Move> previous_move = std::nullopt,
                bool allow_null_pruning = true,
                int check_extensions_remaining = kMaximumCheckExtensionsPerPath,
                Move excluded_move = Move::no_move()) {
        const bool excluded_search = !excluded_move.is_no_move();
        if (ply == 0 && !excluded_search) {
            root_move_score_count = 0;
        }
        if (depth <= 0) {
            return quiescence(state, alpha, beta, ply, 0, previous_move);
        }
        if (ply >= static_cast<int>(SearchStack::kCapacity) - 2) {
            return state.in_check() ? quiescence(state, alpha, beta, ply, 0, previous_move) :
                                      evaluate(state, state.side_to_move());
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
        frame.cutoff_count = 0;
        frame.static_eval_valid = false;
        frame.prior_fail_high = false;
        frame.tt_pv = false;
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
        if (excluded_search) {
            // MoveMetadataList is a fixed container; compact the excluded move
            // without allocating and without exposing an alternate generator.
            std::size_t write_index = 0;
            const std::size_t original_move_count = moves.size();
            for (std::size_t read_index = 0; read_index < original_move_count; ++read_index) {
                if (moves[read_index].move != excluded_move) {
                    moves[write_index++] = moves[read_index];
                }
            }
            moves.resize(write_index);
        }
        if (moves.empty()) {
            return excluded_search ? alpha : terminal_score(state, moves.size(), ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }
        const bool short_timed_root = ply == 0 &&
            time_manager.time_budget().has_value() &&
            *time_manager.time_budget() < kShortTimedFallbackMinimum;
        int child_check_extensions_remaining = check_extensions_remaining;
        const bool check_extension_applied = SearchPolicy::check_extension(
            checked, depth, short_timed_root, check_extensions_remaining);
        if (check_extension_applied) {
            ++stats.check_extensions;
            ++depth;
            --child_check_extensions_remaining;
            frame.extension = 1;
        }
        // Mate-distance pruning keeps a delayed mate from displacing a mate
        // already found nearer to the root. A checked node with an available
        // extension gets the tactical horizon first; otherwise the tightened
        // mate window can discard the very checking line the extension is
        // meant to inspect.
        if (!excluded_search && ply > 0) {
            alpha = std::max(alpha, -kMateScore + ply);
            beta = std::min(beta, kMateScore - ply - 1);
            if (alpha >= beta) {
                return alpha;
            }
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
        bool static_eval_valid = false;
        if (checked) {
            const int previous_ply = ply - 2;
            if (previous_ply >= 0 && stack.frame(static_cast<std::size_t>(previous_ply))
                    .static_eval_valid) {
                static_eval = stack.frame(static_cast<std::size_t>(previous_ply)).static_eval;
                static_eval_valid = true;
            }
        } else {
            static_eval = evaluate(state, state.side_to_move());
            static_eval_valid = true;
        }
        frame.static_eval = static_eval;
        frame.static_eval_valid = static_eval_valid;
        const bool pv_node = beta - alpha > 1;
        const bool cut_node = !pv_node;
        const SearchFrame& parent_frame = ply > 0 ?
            stack.frame(static_cast<std::size_t>(ply - 1)) : frame;
        const SearchFrame& grandparent_frame = ply > 1 ?
            stack.frame(static_cast<std::size_t>(ply - 2)) : frame;
        const bool improving = static_eval_valid && grandparent_frame.static_eval_valid &&
            static_eval > grandparent_frame.static_eval;
        const bool opponent_worsening = static_eval_valid && parent_frame.static_eval_valid &&
            static_eval > -parent_frame.static_eval;
        const bool search_improving = improving || opponent_worsening;
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
            if (depth == 1 && alpha > -kInfinity && static_eval + 120 <= alpha) {
                const int razor_score = quiescence(state, alpha, beta, ply, 0,
                                                   previous_move);
                if (!aborted && razor_score <= alpha) {
                    ++stats.razoring_prunes;
                    return razor_score;
                }
            }
        }

        const int original_alpha = alpha;
        const int original_beta = beta;
        std::optional<Move> tt_move;
        std::optional<TranspositionEntry> tt_entry;
        if (const auto entry = table_access.probe(state.position_key(), ply); entry.has_value()) {
            tt_entry = entry;
            ++stats.tt_hits;
            if (!entry->best_move.is_no_move() && entry->best_move != excluded_move) {
                tt_move = entry->best_move;
            }
            frame.tt_pv = entry->bound == TranspositionBound::exact;
            if (!excluded_search && ply > 0 && entry->depth >= depth) {
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

        const NullMoveDecision null_move_gate = SearchPolicy::null_move(
            depth, alpha, beta, checked, allow_null_pruning && !excluded_search);
        if (null_move_gate.eligible && state.is_repetition_sensitive()) {
            ++stats.null_repetition_skips;
        }
        const bool pawn_endgame = !state.has_non_pawn_material(state.side_to_move()) ||
            !state.has_non_pawn_material(opposite(state.side_to_move()));
        const DynamicNullMoveDecision null_move_decision = SearchPolicy::dynamic_null_move(
            depth, alpha, beta, static_eval, checked,
            allow_null_pruning && !excluded_search && !state.is_repetition_sensitive(),
            search_improving, pawn_endgame);
        if (null_move_decision.eligible && null_move_is_safe(state, ensure_features())) {
            if (state.make_null_move()) {
                PrincipalVariation null_pv;
                const int null_depth = std::max(
                    0, depth - 1 - null_move_decision.reduction);
                const int null_score = -negamax(
                    state, null_depth, -beta, -beta + 1, ply + 1, null_pv,
                    std::nullopt, allow_null_pruning, child_check_extensions_remaining);
                state.unmake_null_move();
                if (aborted) {
                    return 0;
                }
                if (null_score >= beta) {
                    int verified_score = null_score;
                    if (null_move_decision.verify) {
                        ++stats.null_verifications;
                        PrincipalVariation verification_pv;
                        const bool saved_table_state = table_access.enabled();
                        table_access.set_enabled(false);
                        try {
                            verified_score = negamax(
                                state, depth - 1, alpha, beta, ply, verification_pv,
                                previous_move, false, child_check_extensions_remaining);
                        } catch (...) {
                            table_access.set_enabled(saved_table_state);
                            throw;
                        }
                        table_access.set_enabled(saved_table_state);
                        if (aborted) {
                            return 0;
                        }
                    }
                    if (verified_score >= beta && verified_score < kMateThreshold) {
                        ++stats.null_cutoffs;
                        if (!excluded_search) {
                            table_access.store(state.position_key(), depth, verified_score,
                                               TranspositionBound::lower, Move::no_move(), ply);
                        }
                        return verified_score;
                    }
                }
            }
        }

        const SearchHistoryContext history = history_context(
            ply, previous_move, state.position_key());

        // ProbCut is deliberately confined to scout/cut nodes and to
        // positions with a reliable static margin. A capture must first hold
        // in qsearch and then hold in a reduced regular search; this mirrors
        // Stockfish 19's two-stage tactical verification and avoids turning a
        // speculative capture into a PV score.
        const ProbCutDecision probcut = SearchPolicy::prob_cut(
            depth, alpha, beta, static_eval, checked, search_improving,
            excluded_search, state.is_repetition_sensitive());
        if (probcut.eligible && cut_node && !state.is_repetition_sensitive()) {
            MoveMetadataList probcut_moves;
            for (const MoveMetadata& candidate : moves) {
                if ((candidate.is_capture() || candidate.move.promotion() != Promotion::none) &&
                    candidate.move != excluded_move) {
                    (void)probcut_moves.push_back(candidate);
                }
            }
            ordering.order(state, probcut_moves, tt_move, history);
            for (const MoveMetadata& candidate : probcut_moves) {
                if (interrupted()) {
                    return 0;
                }
                ++stats.probcut_searches;
                if (candidate.is_capture() && candidate.see_score < -96 &&
                    !candidate.gives_check) {
                    continue;
                }
                if (!state.make_search_move(candidate)) {
                    continue;
                }
                PrincipalVariation probcut_pv;
                int probcut_score = -quiescence(
                    state, -probcut.beta, -probcut.beta + 1, ply + 1, 0, candidate.move);
                if (!aborted && probcut_score >= probcut.beta && probcut.depth > 0) {
                    probcut_pv = {};
                    probcut_score = -negamax(
                        state, probcut.depth, -probcut.beta, -probcut.beta + 1,
                        ply + 1, probcut_pv, candidate.move, false,
                        child_check_extensions_remaining);
                }
                state.unmake_move();
                if (aborted) {
                    return 0;
                }
                if (probcut_score >= probcut.beta) {
                    ++stats.probcut_cutoffs;
                    if (!excluded_search) {
                        table_access.store(state.position_key(), depth, probcut_score,
                                           TranspositionBound::lower, candidate.move, ply);
                    }
                    return probcut_score - (probcut.beta - beta);
                }
            }
        }

        ordering.order(state, moves, tt_move, history);
        int best_score = -kInfinity;
        Move best_move = Move::no_move();
        int move_number = 0;
        bool singular_probe_done = false;
        const bool singular_candidate = !excluded_search && !checked && ply > 0 &&
            depth >= 6 && !state.is_repetition_sensitive() && tt_move.has_value() &&
            tt_entry.has_value() && tt_entry->bound == TranspositionBound::lower &&
            tt_entry->depth >= depth - 3 &&
            std::abs(tt_entry->score) < kMateThreshold;
        std::optional<PositionFeatures> lmr_parent_features;
        for (const MoveMetadata& metadata : moves) {
            if (interrupted()) {
                return 0;
            }
            const Move move = metadata.move;
            if (move == excluded_move) {
                continue;
            }
            frame.current_move = move;
            const Color moving_side = history_side(state.side_to_move(), false);
            const int history_score = metadata.is_capture() ? 0 :
                ordering.quiet_history_score(moving_side, metadata, history);
            const bool is_tt_move = tt_move.has_value() && move == *tt_move;
            const bool root_pawn_move = ply == 0 && metadata.moving_piece == PieceType::pawn;
            const int full_child_depth = depth - 1;
            const LateMoveDecision lmr_gate = SearchPolicy::dynamic_late_move(
                depth, move_number, full_child_depth, history_score, history_score / 2,
                ordering.capture_history_score(metadata), root_pawn_move,
                checked, metadata.gives_check, metadata.is_capture(),
                move.promotion() != Promotion::none, is_tt_move,
                ordering.is_killer(move, ply), false, false,
                pv_node, cut_node, search_improving,
                ply + 1 < static_cast<int>(SearchStack::kCapacity) &&
                    stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count > 0,
                tt_move.has_value());
            if (lmr_gate.high_history_exclusion) {
                ++stats.lmr_high_history_exclusions;
            }
            const bool lmr_candidate = lmr_gate.candidate && !lmr_gate.high_history_exclusion;
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
            int singular_extension = 0;
            if (singular_candidate && move == *tt_move && !singular_probe_done) {
                singular_probe_done = true;
                ++stats.singular_searches;
                const int singular_margin = 32 + depth * 8 + (pv_node ? 16 : 0);
                const int singular_beta = tt_entry->score - singular_margin;
                const int singular_depth = std::max(1, (depth - 1) / 2);
                PrincipalVariation singular_pv;
                const int excluded_score = negamax(
                    state, singular_depth, singular_beta - 1, singular_beta, ply,
                    singular_pv, previous_move, false,
                    child_check_extensions_remaining, move);
                if (aborted) {
                    return 0;
                }
                if (excluded_score < singular_beta) {
                    singular_extension = excluded_score < singular_beta - 96 ? 2 : 1;
                    ++stats.singular_extensions;
                } else if (!pv_node && depth >= 8 && excluded_score >= beta &&
                           excluded_score < kMateThreshold) {
                    ++stats.multi_cut_prunes;
                    return excluded_score;
                } else if (depth >= 8 &&
                           (tt_entry->score >= beta || cut_node)) {
                    // If the alternative moves are not singular, the TT move
                    // is still useful but need not receive full depth.
                    singular_extension = -1;
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
            const LateMoveDecision lmr_decision = SearchPolicy::dynamic_late_move(
                depth, move_number, full_child_depth, history_score, history_score / 2,
                ordering.capture_history_score(metadata), root_pawn_move,
                checked, metadata.gives_check, metadata.is_capture(),
                move.promotion() != Promotion::none, is_tt_move,
                ordering.is_killer(move, ply), reducible_quiet, quiet_forcing_extension,
                pv_node, cut_node, search_improving,
                ply + 1 < static_cast<int>(SearchStack::kCapacity) &&
                    stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count > 0,
                tt_move.has_value());
            const bool reduced = lmr_decision.reduced;
            const int reduction = lmr_decision.reduction;
            frame.reduction = reduced ? reduction : 0;
            const int extension_depth = singular_extension + (quiet_forcing_extension ? 1 : 0);
            const int authoritative_child_depth = std::max(
                0, full_child_depth + extension_depth);
            const int child_depth = reduced ?
                std::max(0, authoritative_child_depth - reduction) : authoritative_child_depth;
            if (reduced) {
                ++stats.lmr_reductions;
            }
            // Child futility is a narrow scout-node optimisation. It is
            // intentionally unavailable to the first move, PV nodes,
            // forcing moves, and TT moves, so a quiet move that is the only
            // plausible continuation still receives an authoritative search.
            const bool child_futility = !pv_node && !checked && depth <= 3 &&
                !tactical_position && move_number > 0 && !metadata.is_capture() &&
                !metadata.gives_check &&
                move.promotion() == Promotion::none && !is_tt_move &&
                static_eval + 96 + depth * 72 <= alpha;
            if (child_futility) {
                ++stats.quiet_futility_prunes;
                state.unmake_move();
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            if (SearchPolicy::quiet_futility(
                    phase_rich_quiet_position, metadata.is_capture(), metadata.gives_check,
                    move.promotion() != Promotion::none, move_number, static_eval, depth, alpha)) {
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
                        state, authoritative_child_depth, -beta, -alpha, ply + 1, child_pv,
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
                if (ply > 0 && !metadata.is_capture() &&
                    move.promotion() == Promotion::none) {
                    const Color history_side_after_unmake = history_side(moving_side, true);
                    ordering.record_quiet_best(history_side_after_unmake, metadata, depth, history);
                } else if (ply > 0 && metadata.is_capture()) {
                    ordering.record_capture_best(metadata, depth, history);
                    ++stats.capture_history_updates;
                }
            }
            if (ply == 0 && root_move_score_count < root_move_scores.size()) {
                root_move_scores[root_move_score_count++] = RootMoveScore{move, score};
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                frame.cutoff_count++;
                frame.prior_fail_high = true;
                if (ply > 0 && !metadata.is_capture() && move.promotion() == Promotion::none) {
                    const Color history_side_after_unmake = history_side(moving_side, true);
                    ordering.record_quiet_cutoff(history_side_after_unmake, metadata, depth,
                                                 history);
                    ++stats.quiet_history_updates;
                    if (previous_move.has_value()) {
                        ++stats.continuation_history_updates;
                    }
                } else if (ply > 0 && metadata.is_capture()) {
                    ordering.record_capture_cutoff(metadata, depth, history);
                    ++stats.capture_history_updates;
                }
                break;
            }
            if (ply > 0 && !metadata.is_capture() && move.promotion() == Promotion::none) {
                const Color history_side_after_unmake = history_side(moving_side, true);
                ordering.record_quiet_fail(history_side_after_unmake, metadata, depth, history);
                ++stats.quiet_history_updates;
                if (previous_move.has_value()) {
                    ++stats.continuation_history_updates;
                }
            } else if (ply > 0 && metadata.is_capture()) {
                ordering.record_capture_fail(metadata, depth, history);
                ++stats.capture_history_updates;
            }
            ++move_number;
            frame.move_count = move_number;
        }

        if (best_score == -kInfinity) {
            best_score = static_eval_valid ? static_eval : 0;
        }
        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= original_beta ? TranspositionBound::lower : TranspositionBound::exact;
        if (!excluded_search) {
            table_access.store(state.position_key(), depth, best_score, bound, best_move, ply);
        }
        return best_score;
    }
};

} // namespace koi::detail
