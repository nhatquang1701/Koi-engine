#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    // History maluses are deferred until a node has selected its final best
    // move.  They do not need SEE, provenance, or ordering data; retaining a
    // compact record keeps the recursive stack bounded when a node has many
    // legal moves while preserving the exact metadata dimensions used by the
    // ordering tables.
    struct DeferredHistoryMove {
        Move move = Move::no_move();
        PieceType moving_piece = PieceType::none;
        PieceType captured_piece = PieceType::none;
        MoveKind kind = MoveKind::quiet;
        Color history_side = Color::white;

        [[nodiscard]] static DeferredHistoryMove from(
            const MoveMetadata& metadata, const Color side = Color::white) noexcept {
            return DeferredHistoryMove{
                metadata.move, metadata.moving_piece, metadata.captured_piece,
                metadata.kind, side};
        }

        [[nodiscard]] MoveMetadata metadata() const noexcept {
            MoveMetadata result;
            result.move = move;
            result.moving_piece = moving_piece;
            result.captured_piece = captured_piece;
            result.kind = kind;
            return result;
        }
    };

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
    // A previous completed root move remains a useful ordering hint even when
    // the shared TT entry was displaced or hash use is disabled. The service
    // sets this only between completed iterations; the first iteration keeps
    // the normal generated-move ordering.
    std::optional<Move> root_move_hint;

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
        // Qsearch can be entered before regular negamax initializes the frame
        // at this ply, and its recursive children enter the same way.  Keep
        // the move that led here in the frame so history_context() can retain
        // the multi-ply continuation suffix on the next qsearch call.
        SearchFrame& qframe = stack.frame(static_cast<std::size_t>(std::max(ply, 0)));
        qframe.previous_move = previous_move.value_or(Move::no_move());
        qframe.in_check = checked;
        qframe.move_count = 0;
        qframe.reduction = 0;
        qframe.cutoff_count = 0;
        qframe.prior_fail_high = false;
        std::optional<TranspositionEntry> tt_entry;
        if (const auto entry = table_access.probe(state.position_key(), ply); entry.has_value()) {
            tt_entry = entry;
            ++stats.tt_hits;
        }
        MoveMetadataList moves;
        const bool has_legal_move = checked ?
            (state.legal_moves_with_metadata(moves, true, false), !moves.empty()) :
            state.legal_tactical_moves_with_metadata(
                moves, qdepth < quiescence_check_depth_limit, false);
        if (!has_legal_move) {
            return terminal_score(state, 0, ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }

        // Qsearch move generation and pruning depend on qdepth and on the
        // move that led into the node. Neither dimension is in position_key(),
        // so qsearch TT entries are ordering hints only. In particular, do not
        // reuse a depth-zero bound computed at a different tactical frontier.

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
                                                               state.pawn_key());
        std::optional<Move> tt_move;
        if (tt_entry.has_value() && !tt_entry->best_move.is_no_move()) {
            tt_move = tt_entry->best_move;
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
            // A checked position has no stand-pat score.  Stop at the hard
            // safety horizon before creating another large qsearch frame;
            // this boundary exists specifically to prevent perpetual-check
            // cycles from exhausting the native call stack.
            return 0;
        }

        SearchMovePicker picker(
            ordering, state, moves, tt_move, history, ply,
            checked ? SearchMovePicker::Mode::evasion : SearchMovePicker::Mode::quiescence);
        const Square previous_destination = previous_move.has_value() ?
            previous_move->to() : Square::from_index(Square::kInvalid);
        const int futility_base = !checked && best > -kMateThreshold ?
            best + kQuiescenceFutilityMargin : -kInfinity;
        int move_count = 0;
        while (const auto candidate = picker.next()) {
            const MoveMetadata& metadata = *candidate;
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
            const bool capture = metadata.is_capture();
            const bool promotion = move.promotion() != Promotion::none;
            const bool protects_previous_destination = previous_move.has_value() &&
                metadata.move.to() == previous_destination;
            ++move_count;

            // Follow Stockfish 19's qsearch ordering of cheap pruning: the
            // previous move's destination is protected from futility, the
            // first two tactical candidates get a futility/SEE test, and a
            // final SEE floor keeps poisoned captures out of the frontier.
            // Checked nodes have no stand-pat bound and therefore search all
            // generated evasions.
            if (!checked && best > -kMateThreshold) {
                if (!metadata.gives_check && !protects_previous_destination &&
                    futility_base > -kMateThreshold && !promotion) {
                    if (move_count > kQuiescenceFutilityMoveLimit) {
                        ++stats.delta_prunes;
                        continue;
                    }
                    const int futility_value =
                        futility_base + piece_value(metadata.captured_piece);
                    if (futility_value <= alpha) {
                        best = std::max(best, futility_value);
                        ++stats.delta_prunes;
                        continue;
                    }
                    if (capture && metadata.see_score < alpha - futility_base) {
                        best = std::max(best, std::min(alpha, futility_base));
                        ++stats.see_prunes;
                        continue;
                    }
                }

                // Quiet checks remain part of Koi's bounded qsearch
                // extension; ordinary quiet moves are not in the tactical
                // generator and are rejected defensively here.
                if (!capture && !metadata.gives_check && !promotion) {
                    continue;
                }
                if (capture && metadata.see_score < kQuiescenceSeeThreshold) {
                    ++stats.see_prunes;
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
            const int score = -quiescence(state, -beta, -alpha, ply + 1, qdepth + 1,
                                          metadata.move);
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
        // Do not store a qsearch bound here. The qsearch frontier is shaped by
        // qdepth, previous_move, and selective SEE/delta pruning, while the TT
        // key contains only the position. Regular-search capture history is
        // likewise updated only by full-search nodes, where its semantics are
        // compatible with the capture-pruning margins.
        return best;
    }

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv, std::optional<Move> previous_move = std::nullopt,
                bool allow_null_pruning = true,
                int check_extensions_remaining = kMaximumCheckExtensionsPerPath,
                Move excluded_move = Move::no_move()) {
        const bool excluded_search = !excluded_move.is_no_move();
        SearchFrame& parent_frame = ply > 0 ?
            stack.frame(static_cast<std::size_t>(ply - 1)) : stack.frame(0);
        const int prior_reduction = (!excluded_search && ply > 0) ?
            parent_frame.reduction : 0;
        // A reduction belongs to exactly one real child. Consume it before
        // depth-boundary returns too, because a reduced child can enter
        // qsearch without reaching the normal frame setup below. Excluded
        // singular probes run at the parent ply and must not consume the
        // parent's pending reduction.
        if (!excluded_search && ply > 0) {
            parent_frame.reduction = 0;
        }
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
        const bool repetition_sensitive = state.is_repetition_sensitive();
        const Color side_to_move = state.side_to_move();
        const bool parent_has_non_pawn_material =
            state.has_non_pawn_material(side_to_move);
        SearchFrame& frame = stack.frame(static_cast<std::size_t>(std::max(ply, 0)));
        // Singular probes recurse at the same ply while temporarily excluding
        // the current TT move.  Stockfish 19 keeps the parent node's
        // static-evaluation and TT-PV snapshot for that probe; preserve it
        // before the ordinary node initialization below so the reduced
        // alternative search does not change its own reference point.
        const SearchFrame inherited_frame = frame;
        frame.previous_move = previous_move.value_or(Move::no_move());
        frame.in_check = checked;
        frame.move_count = 0;
        frame.reduction = 0;
        frame.extension = 0;
        frame.cutoff_count = 0;
        frame.static_eval = excluded_search ? inherited_frame.static_eval : 0;
        frame.static_eval_valid = excluded_search ? inherited_frame.static_eval_valid : false;
        frame.prior_fail_high = false;
        frame.tt_pv = excluded_search ? inherited_frame.tt_pv : false;
        if (ply + 2 < static_cast<int>(SearchStack::kCapacity)) {
            // The current node reads the cutoff count owned by its immediate
            // child. Clear the grandchild slot instead, matching Stockfish's
            // stack ownership: the child entry initializes its own frame and
            // its completed search then leaves the signal for later siblings.
            stack.frame(static_cast<std::size_t>(ply + 2)).cutoff_count = 0;
        }
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
        } else if (excluded_search) {
            static_eval = inherited_frame.static_eval;
            static_eval_valid = inherited_frame.static_eval_valid;
        } else {
            static_eval = evaluate(state, state.side_to_move());
            static_eval_valid = true;
        }
        frame.static_eval = static_eval;
        frame.static_eval_valid = static_eval_valid;
        const bool pv_node = beta - alpha > 1;
        const bool cut_node = !pv_node;
        const SearchFrame& grandparent_frame = ply > 1 ?
            stack.frame(static_cast<std::size_t>(ply - 2)) : frame;
        bool improving = static_eval_valid && grandparent_frame.static_eval_valid &&
            static_eval > grandparent_frame.static_eval;
        const bool opponent_worsening = static_eval_valid && parent_frame.static_eval_valid &&
            static_eval > -parent_frame.static_eval;
        bool search_improving = improving || opponent_worsening;
        // Hindsight compensation is one of Stockfish 19's safeguards around
        // LMR: a reduced child that makes the static position worse deserves
        // a little extra depth, while a reduced child that improves the
        // evaluation need not be paid back.  Keep it outside excluded
        // singular probes, whose depth is intentionally controlled by the
        // singular gate itself.
        if (!excluded_search && static_eval_valid && parent_frame.static_eval_valid) {
            if (prior_reduction >= 3 && !opponent_worsening &&
                depth < kMaximumSearchDepth) {
                ++depth;
            }
            if (prior_reduction >= 2 && depth >= 2 &&
                static_eval + parent_frame.static_eval > 166) {
                --depth;
            }
        }
        if (!excluded_search) {
            frame.tt_pv = pv_node;
        }
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
            if (!excluded_search) {
                frame.tt_pv = pv_node || entry->bound == TranspositionBound::exact;
            }
            if (!excluded_search && !repetition_sensitive && !pv_node && ply > 0 &&
                entry->depth >= depth) {
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
        if (ply == 0 && !excluded_search && root_move_hint.has_value() &&
            std::find_if(moves.begin(), moves.end(), [this](const MoveMetadata& metadata) {
                return metadata.move == *root_move_hint;
            }) != moves.end()) {
            tt_move = root_move_hint;
        }

        const NullMoveDecision null_move_gate = SearchPolicy::null_move(
            depth, alpha, beta, checked, allow_null_pruning && !excluded_search);
        if (null_move_gate.eligible && repetition_sensitive) {
            ++stats.null_repetition_skips;
        }
        const bool pawn_endgame = !state.has_non_pawn_material(state.side_to_move()) ||
            !state.has_non_pawn_material(opposite(state.side_to_move()));
        const DynamicNullMoveDecision null_move_decision = SearchPolicy::dynamic_null_move(
            depth, alpha, beta, static_eval, checked,
            allow_null_pruning && !excluded_search && !repetition_sensitive,
            search_improving, pawn_endgame);
        if (null_move_decision.eligible && null_move_is_safe(state, ensure_features())) {
            if (state.make_null_move()) {
                PrincipalVariation null_pv;
                const int null_depth = std::max(
                    0, depth - null_move_decision.reduction);
                const int null_score = -negamax(
                    state, null_depth, -beta, -beta + 1, ply + 1, null_pv,
                    std::optional<Move>{Move::no_move()}, allow_null_pruning,
                    child_check_extensions_remaining);
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
                        const SearchFrame saved_frame = frame;
                        const bool has_probe_child_frame =
                            ply + 1 < static_cast<int>(SearchStack::kCapacity);
                        const SearchFrame saved_probe_child_frame = has_probe_child_frame ?
                            stack.frame(static_cast<std::size_t>(ply + 1)) : SearchFrame{};
                        table_access.set_enabled(false);
                        try {
                            verified_score = negamax(
                                state, null_depth, beta - 1, beta, ply, verification_pv,
                                previous_move, false, child_check_extensions_remaining);
                        } catch (...) {
                            frame = saved_frame;
                            if (has_probe_child_frame) {
                                stack.frame(static_cast<std::size_t>(ply + 1)) =
                                    saved_probe_child_frame;
                            }
                            table_access.set_enabled(saved_table_state);
                            throw;
                        }
                        frame = saved_frame;
                        if (has_probe_child_frame) {
                            stack.frame(static_cast<std::size_t>(ply + 1)) =
                                saved_probe_child_frame;
                        }
                        table_access.set_enabled(saved_table_state);
                        if (aborted) {
                            return 0;
                        }
                    }
                    if (verified_score >= beta && verified_score < kMateThreshold) {
                        ++stats.null_cutoffs;
                        if (!excluded_search) {
                            // The null result was proven only at its reduced
                            // verification horizon. Storing it at the parent
                            // depth would make a speculative bound look like
                            // a full-depth search to later nodes.
                            table_access.store(state.position_key(), null_depth, null_score,
                                               TranspositionBound::lower, Move::no_move(), ply);
                        }
                        return null_score;
                    }
                }
            }
        }

        const SearchHistoryContext history = history_context(
            ply, previous_move, state.pawn_key());

        // Stockfish 19 folds a static fail-high into the improving state after
        // the null-move probe. That state feeds IIR, ProbCut, and LMR; keeping
        // the update after verification avoids making the null gate itself
        // more permissive.
        improving = improving || (static_eval_valid && static_eval >= beta);
        search_improving = improving || opponent_worsening;

        // Internal iterative reduction is useful only when the node has no
        // move hint to establish a trustworthy search order. Keep the
        // reduction off PV nodes and tactical/repetition-sensitive positions:
        // those nodes are precisely where a missing TT move is least
        // informative and where a one-ply horizon change can hide a forcing
        // continuation. This is the conservative counterpart of Stockfish
        // 19's no-TT IIR gate.
        if (!excluded_search && ply > 0 && !pv_node && !checked &&
            !tactical_position && !state.is_repetition_sensitive() &&
            !tt_move.has_value() && depth >= kInternalIterativeReductionMinimumDepth) {
            --depth;
        }

        // ProbCut is deliberately confined to scout/cut nodes and to
        // positions with a reliable static margin. A capture must first hold
        // in qsearch and then hold in a reduced regular search; this mirrors
        // Stockfish 19's two-stage tactical verification and avoids turning a
        // speculative capture into a PV score.
        const ProbCutDecision probcut = SearchPolicy::prob_cut(
            depth, alpha, beta, static_eval, checked, search_improving,
            excluded_search, repetition_sensitive);
        if (probcut.eligible && cut_node && !repetition_sensitive && !pawn_endgame &&
            !(tt_entry.has_value() && tt_entry->score < probcut.beta)) {
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
                const bool candidate_gives_check = candidate.gives_check ||
                    (candidate.is_capture() && state.move_gives_check(candidate.move));
                if (candidate.is_capture() &&
                    candidate.see_score < probcut.beta - static_eval &&
                    !candidate_gives_check) {
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
                        // The lower bound was established at the reduced
                        // ProbCut horizon, not at the parent depth. Keeping
                        // the real proof depth prevents a later full search
                        // from treating this selective result as exact.
                        table_access.store(state.position_key(), probcut.depth > 0 ?
                                           probcut.depth + 1 : 0, probcut_score,
                                           TranspositionBound::lower, candidate.move, ply);
                    }
                    if (std::abs(probcut_score) < kMateThreshold) {
                        return probcut_score - (probcut.beta - beta);
                    }
                }
            }
        }

        // A sufficiently deep lower TT bound can act as Stockfish 19's
        // second, deliberately soft ProbCut. The returned value is kept at a
        // bounded margin above beta rather than exposing a stale TT score as
        // exact, and mate/repetition-sensitive positions remain on the full
        // move loop.
        const bool tt_probcut = !excluded_search && !pv_node && !checked && !pawn_endgame &&
            !state.is_repetition_sensitive() && tt_entry.has_value() &&
            tt_entry->bound == TranspositionBound::lower &&
            tt_entry->depth >= depth - 4 &&
            tt_entry->score >= beta + kTranspositionProbCutMargin &&
            std::abs(beta) < kMateThreshold &&
            std::abs(tt_entry->score) < kMateThreshold;
        if (tt_probcut) {
            return beta + kTranspositionProbCutMargin;
        }

        SearchMovePicker picker(
            ordering, state, moves, tt_move, history, ply,
            checked ? SearchMovePicker::Mode::evasion : SearchMovePicker::Mode::main,
            excluded_move);
        int best_score = -kInfinity;
        Move best_move = Move::no_move();
        int move_number = 0;
        bool prior_child_fail_high = false;
        bool singular_probe_done = false;
        std::array<DeferredHistoryMove, kMaximumLegalMoves> failed_moves{};
        std::size_t failed_move_count = 0;
        MoveMetadata best_metadata{};
        Color best_history_side = side_to_move;
        bool best_metadata_valid = false;
        bool best_move_cutoff = false;
        bool selective_pruning = false;
        const bool singular_candidate = !excluded_search && !checked && !pawn_endgame && ply > 0 &&
            depth >= 6 + (frame.tt_pv ? 1 : 0) && !repetition_sensitive &&
            tt_move.has_value() &&
            tt_entry.has_value() && tt_entry->bound == TranspositionBound::lower &&
            tt_entry->depth >= depth - 3 &&
            std::abs(tt_entry->score) < kMateThreshold;
        std::optional<PositionFeatures> lmr_parent_features;
        while (const auto candidate = picker.next()) {
            const MoveMetadata& metadata = *candidate;
            if (interrupted()) {
                return 0;
            }
            const Move move = metadata.move;
            frame.current_move = move;
            // Once a cut node has already examined enough candidates, keep
            // Stockfish's staged-picker behavior: tactical moves and the
            // strongest quiet history are still available, while the tail of
            // unpromising quiets is deferred out of this node. Root and PV
            // nodes retain every candidate so a partial ordering decision can
            // never replace an authoritative principal variation.
            const int next_move_number = move_number + 1;
            const int quiet_skip_threshold =
                (3 + depth * depth) / (2 - static_cast<int>(improving));
            if (!excluded_search && ply > 0 && !pv_node && !checked &&
                parent_has_non_pawn_material &&
                best_score > -kMateThreshold && next_move_number >= quiet_skip_threshold) {
                picker.skip_quiet_moves();
                selective_pruning = true;
            }
            const Color moving_side = history_side(state.side_to_move(), false);
            const int history_score = metadata.is_capture() ? 0 :
                ordering.quiet_history_score(moving_side, metadata, history);
            const bool is_tt_move = tt_move.has_value() && move == *tt_move;
            const bool root_pawn_move = ply == 0 && metadata.moving_piece == PieceType::pawn;
            const int full_child_depth = depth - 1;
            const LateMoveDecision lmr_gate = excluded_search ? LateMoveDecision{} :
                SearchPolicy::dynamic_late_move(
                    depth, move_number, full_child_depth, history_score,
                    ordering.continuation_history_score(metadata, history),
                    ordering.capture_history_score(metadata), root_pawn_move,
                    checked, metadata.gives_check, metadata.is_capture(),
                    move.promotion() != Promotion::none, is_tt_move,
                    ordering.is_killer(move, ply), false, false,
                    pv_node, cut_node, search_improving, prior_child_fail_high,
                    ply + 1 < static_cast<int>(SearchStack::kCapacity) ?
                        stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count : 0,
                    tt_move.has_value(), frame.tt_pv);
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
                const int singular_margin =
                    (59 + 66 * (frame.tt_pv && !pv_node)) * depth / 63;
                const int singular_beta = tt_entry->score - singular_margin;
                const int singular_depth = std::max(1, (depth - 1) / 2);
                PrincipalVariation singular_pv;
                const SearchFrame saved_frame = frame;
                const bool has_probe_child_frame =
                    ply + 1 < static_cast<int>(SearchStack::kCapacity);
                const SearchFrame saved_probe_child_frame = has_probe_child_frame ?
                    stack.frame(static_cast<std::size_t>(ply + 1)) : SearchFrame{};
                int excluded_score = 0;
                try {
                    excluded_score = negamax(
                        state, singular_depth, singular_beta - 1, singular_beta, ply,
                        singular_pv, previous_move, false,
                        child_check_extensions_remaining, move);
                } catch (...) {
                    frame = saved_frame;
                    if (has_probe_child_frame) {
                        stack.frame(static_cast<std::size_t>(ply + 1)) =
                            saved_probe_child_frame;
                    }
                    throw;
                }
                frame = saved_frame;
                if (has_probe_child_frame) {
                    stack.frame(static_cast<std::size_t>(ply + 1)) =
                        saved_probe_child_frame;
                }
                if (aborted) {
                    return 0;
                }
                if (excluded_score < singular_beta) {
                    // Use two calibrated margins, following Stockfish 19's
                    // one/two/three-ply singular-extension ladder. A PV
                    // node and a quiet TT move receive slightly more room;
                    // captures already carry independent SEE protection and
                    // should not be extended merely because they are noisy.
                    const int double_margin = std::max(
                        16, 64 + (pv_node ? 32 : 0) - (metadata.is_capture() ? 0 : 16));
                    const int triple_margin = std::max(
                        double_margin + 16,
                        144 + (pv_node ? 48 : 0) - (metadata.is_capture() ? 0 : 24));
                    singular_extension = 1 +
                        (excluded_score < singular_beta - double_margin ? 1 : 0) +
                        (excluded_score < singular_beta - triple_margin ? 1 : 0);
                    ++stats.singular_extensions;
                } else if (!pv_node && depth >= 8 && excluded_score >= beta &&
                           excluded_score < kMateThreshold) {
                    ++stats.multi_cut_prunes;
                    return excluded_score;
                } else if (depth >= 8 &&
                           (tt_entry->score >= beta || cut_node)) {
                    // If the alternative moves are not singular, the TT move
                    // is still useful but need not receive full depth.
                    singular_extension = -3;
                }
            }
            if (!state.make_search_move(metadata)) {
                // A failed metadata application should be impossible for a
                // generator-owned candidate, but it still consumed one move
                // slot. Keep move-number-dependent pruning deterministic if a
                // defensive validation check rejects it.
                selective_pruning = true;
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            bool king_zone_pressure = false;
            bool reducible_quiet = false;
            if (lmr_candidate) {
                if (metadata.is_capture()) {
                    // A non-checking capture has no quiet-move forcing
                    // features to inspect. Avoid paying for a feature rebuild
                    // on the tactical branch; its SEE/capture history already
                    // supplies the safety signal used by LMR.
                    reducible_quiet = !state.in_check();
                } else if (lmr_parent_features.has_value()) {
                    ++stats.position_feature_extractions;
                    const PositionFeatures after_quiet_features = state.position_features();
                    const std::size_t enemy = lmr_parent_features->side_to_move == Color::white ? 1U : 0U;
                    king_zone_pressure = after_quiet_features.king_zone_attacks[enemy] >
                        lmr_parent_features->king_zone_attacks[enemy];
                    reducible_quiet = !state.in_check() &&
                        !quiet_move_is_forcing(*lmr_parent_features, after_quiet_features, metadata);
                }
            }
            if (king_zone_pressure) {
                ++stats.lmr_king_zone_exclusions;
            }
            if (king_zone_pressure) {
                // A quiet move that increases direct king-ring pressure is a
                // forcing defensive/tactical resource even when its move
                // metadata is not a check. Do not let the later LMR call
                // reduce it after paying for the feature comparison.
                reducible_quiet = false;
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
            const LateMoveDecision lmr_decision = excluded_search ? LateMoveDecision{} :
                SearchPolicy::dynamic_late_move(
                    depth, move_number, full_child_depth, history_score,
                    ordering.continuation_history_score(metadata, history),
                    ordering.capture_history_score(metadata), root_pawn_move,
                    checked, metadata.gives_check, metadata.is_capture(),
                    move.promotion() != Promotion::none, is_tt_move,
                    ordering.is_killer(move, ply), reducible_quiet, quiet_forcing_extension,
                    pv_node, cut_node, search_improving, prior_child_fail_high,
                    ply + 1 < static_cast<int>(SearchStack::kCapacity) ?
                        stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count : 0,
                    tt_move.has_value(), frame.tt_pv);
            const bool reduced = lmr_decision.reduced;
            const int reduction = lmr_decision.reduction;
            const int extension_depth = singular_extension + (quiet_forcing_extension ? 1 : 0);
            const int authoritative_child_depth = std::max(
                0, full_child_depth + extension_depth);
            const int child_depth = reduced ?
                std::max(0, authoritative_child_depth - reduction) : authoritative_child_depth;
            if (reduced) {
                ++stats.lmr_reductions;
            }
            // Mirror Stockfish 19's shallow capture gates at non-PV nodes.
            // The move is already legal and made here so the normal unmake
            // path remains the single state-restoration path. Do not prune a
            // first candidate, a TT move, a checked/repetition-sensitive
            // position, or a sacrifice below the draw score; those cases are
            // too important for mate and defensive resource discovery.
            const int lmr_depth = std::max(0, full_child_depth - (reduced ? reduction : 0));
            const int capture_history_score = ordering.capture_history_score(metadata);
            const bool capture_pruning = !excluded_search && ply > 0 && !pv_node && !checked &&
                !repetition_sensitive && move_number > 0 &&
                best_score > -kMateThreshold && parent_has_non_pawn_material &&
                metadata.is_capture() && !metadata.gives_check && !is_tt_move;
            if (capture_pruning) {
                const bool capture_futility = lmr_depth < 8 &&
                    static_eval + 234 + 247 * lmr_depth +
                        piece_value(metadata.captured_piece) +
                        134 * capture_history_score / 1024 <= alpha;
                const int see_margin = std::max(0, 177 * depth +
                                                    34 * capture_history_score / 1024);
                const bool capture_see = alpha >= 0 && metadata.see_score < -see_margin;
                if (capture_futility || capture_see) {
                    selective_pruning = true;
                    if (capture_see) {
                        ++stats.see_prunes;
                    }
                    state.unmake_move();
                    ++move_number;
                    frame.move_count = move_number;
                    continue;
                }
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
                selective_pruning = true;
                ++stats.quiet_futility_prunes;
                state.unmake_move();
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            if (SearchPolicy::quiet_futility(
                    phase_rich_quiet_position, metadata.is_capture(), metadata.gives_check,
                    move.promotion() != Promotion::none, move_number, static_eval, depth, alpha)) {
                selective_pruning = true;
                ++stats.quiet_futility_prunes;
                state.unmake_move();
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            // Publish the reduction only while an actual child is about to
            // consume it. Pruned candidates never enter negamax/qsearch and
            // therefore must not leak their reduction into the next sibling.
            frame.reduction = reduced ? reduction : 0;
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
            if (ply == 0 && std::getenv("KOI_TRACE_ROOT") != nullptr) {
                const std::string move_text = move.uci();
                std::fprintf(stderr,
                             "root-trace depth=%d move=%s n=%d alpha=%d beta=%d score=%d "
                             "child=%d auth=%d reduced=%d qext=%d check=%d tt=%d\n",
                             depth, move_text.c_str(), move_number, alpha, beta, score,
                             child_depth, authoritative_child_depth, reduced ? 1 : 0,
                             quiet_forcing_extension ? 1 : 0, metadata.gives_check ? 1 : 0,
                             is_tt_move ? 1 : 0);
            }
            prior_child_fail_high = score > alpha;
            const bool quiet_history_move = !metadata.is_capture() &&
                move.promotion() == Promotion::none;
            const Color history_side_after_unmake = quiet_history_move ?
                history_side(moving_side, true) : moving_side;

            if (score > best_score) {
                if (best_metadata_valid && failed_move_count < failed_moves.size()) {
                    failed_moves[failed_move_count++] =
                        DeferredHistoryMove::from(best_metadata, best_history_side);
                }
                best_score = score;
                best_move = move;
                best_metadata = metadata;
                best_metadata_valid = true;
                best_history_side = history_side_after_unmake;
                pv.prepend(move, child_pv);
            } else if (failed_move_count < failed_moves.size()) {
                failed_moves[failed_move_count++] =
                    DeferredHistoryMove::from(metadata, history_side_after_unmake);
            }
            if (ply == 0 && root_move_score_count < root_move_scores.size()) {
                root_move_scores[root_move_score_count++] = RootMoveScore{move, score};
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                frame.cutoff_count++;
                frame.prior_fail_high = true;
                best_move_cutoff = true;
                break;
            }
            ++move_number;
            frame.move_count = move_number;
        }

        // Stockfish updates the selected move and all searched alternatives
        // after the node is known to be complete. Deferring the updates is
        // important here: a move that was temporarily best must receive a
        // failure malus if a later move overtakes it, rather than retaining a
        // positive best-move update from the same node.
        if (ply > 0 && best_metadata_valid) {
            if (best_metadata.is_capture()) {
                if (best_move_cutoff) {
                    ordering.record_capture_cutoff(best_metadata, depth, history);
                } else {
                    ordering.record_capture_best(best_metadata, depth, history);
                }
                ++stats.capture_history_updates;
            } else if (best_metadata.move.promotion() == Promotion::none) {
                if (best_move_cutoff) {
                    ordering.record_quiet_cutoff(best_history_side, best_metadata, depth, history);
                } else {
                    ordering.record_quiet_best(best_history_side, best_metadata, depth, history);
                }
                ++stats.quiet_history_updates;
                if (history.count > 0) {
                    ++stats.continuation_history_updates;
                }
            }
            for (std::size_t index = 0; index < failed_move_count; ++index) {
                const DeferredHistoryMove& failed = failed_moves[index];
                const MoveMetadata failed_metadata = failed.metadata();
                if (failed_metadata.is_capture()) {
                    ordering.record_capture_fail(failed_metadata, depth, history);
                    ++stats.capture_history_updates;
                } else if (failed_metadata.move.promotion() == Promotion::none) {
                    ordering.record_quiet_fail(
                        failed.history_side, failed_metadata, depth, history);
                    ++stats.quiet_history_updates;
                    if (history.count > 0) {
                        ++stats.continuation_history_updates;
                    }
                }
            }
        }

        if (best_score == -kInfinity) {
            best_score = static_eval_valid ? static_eval : 0;
        }
        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= original_beta ? TranspositionBound::lower : TranspositionBound::exact;
        // An exact bound requires at least one authoritative child and no
        // candidate removed by selective pruning. A node that searched no
        // child must not turn its static fallback into an exact TT answer;
        // that value is only a heuristic estimate for the current node.
        const bool safe_to_store = best_metadata_valid &&
            (!selective_pruning || bound == TranspositionBound::lower);
        if (!excluded_search && !repetition_sensitive && safe_to_store) {
            table_access.store(state.position_key(), depth, best_score, bound, best_move, ply);
        }
        return best_score;
    }
};

} // namespace koi::detail
