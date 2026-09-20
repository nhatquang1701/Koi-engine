#include "koi/detail/search_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "koi/syzygy_tablebase.hpp"

namespace koi::detail {

SearchContext::~SearchContext() = default;

static_assert(SearchContext::stack_capacity() == SearchStack::kCapacity);

std::optional<int> SearchContext::probe_tablebase(const GameState& state, const int depth) {
    const auto decisive_score = [](const std::optional<int> mate) -> std::optional<int> {
        if (!mate.has_value() || *mate == 0) {
            // Draws, cursed wins, and blessed losses never cut off.
            return std::nullopt;
        }
        return *mate > 0 ? std::optional<int>{kTablebaseInteriorWinScore} :
            std::optional<int>{kTablebaseInteriorLossScore};
    };

    if (tablebase.probe_hook) {
        if (tablebase.fifty_move_rule && state.halfmove_clock() != 0) {
            // A win that the 50-move counter can dilute is not decisive.  Skip
            // the hook entirely so the diagnostic seam has no false side effects.
            return std::nullopt;
        }
        // The hook is a test/diagnostic seam and must never terminate a search.
        try {
            const std::optional<TablebaseProbeResult> result =
                tablebase.probe_hook(state, depth);
            if (!result.has_value()) {
                return std::nullopt;
            }
            return decisive_score(result->mate);
        } catch (...) {
            return std::nullopt;
        }
    }

    if (tablebase.table == nullptr || !tablebase.table->probe_eligible(state)) {
        return std::nullopt;
    }
    if (tablebase.table->fifty_move_rule() && state.halfmove_clock() != 0) {
        return std::nullopt;
    }
    const std::optional<SyzygyWdl> wdl = tablebase.table->probe_wdl(state.tablebase_snapshot());
    if (!wdl.has_value()) {
        return std::nullopt;
    }
    return decisive_score(syzygy_score(*wdl).mate);
}

int SearchContext::quiescence(GameState& state, int alpha, int beta, int ply,
                              int qdepth,
                              std::optional<Move> previous_move,
                              bool* repetition_sensitive_path,
                              bool* selective_bound_path,
                              bool* lower_bound_path) {
        if (!count_node(true)) {
            return 0;
        }
        record_ply(ply);

        const bool checked = state.in_check();
        const bool repetition_sensitive = state.is_repetition_sensitive();
        bool path_repetition_sensitive = repetition_sensitive;
        bool path_selective_bound = false;
        if (repetition_sensitive_path != nullptr) {
            *repetition_sensitive_path = path_repetition_sensitive;
        }
        if (selective_bound_path != nullptr) {
            *selective_bound_path = false;
        }
        bool path_lower_bound = false;
        if (lower_bound_path != nullptr) {
            *lower_bound_path = false;
        }
        const LowerBoundPathGuard lower_bound_path_guard{
            lower_bound_path, &path_lower_bound};
        const SelectiveBoundPathGuard selective_path_guard{
            selective_bound_path, &path_selective_bound};
        // A regular search near the fixed-stack boundary may enter qsearch
        // with only the final frame available.  Do not let qsearch clamp a
        // deeper recursive ply onto that same frame: terminal and draw
        // handling still has to run before the static horizon fallback.
        if (ply >= static_cast<int>(SearchStack::kCapacity) - 1) {
            const std::vector<Move> legal_moves = state.legal_moves();
            if (legal_moves.empty()) {
                return terminal_score(state, 0, ply);
            }
            const DrawStatus draw_status = state.draw_status();
            if (is_forced_draw_status(draw_status)) {
                return 0;
            }
            // There are legal moves, but the fixed stack boundary prevents
            // this qsearch node from examining its tactical frontier.
            path_selective_bound = true;
            if (checked) {
                return terminal_score(state, legal_moves.size(), ply);
            }
            const int static_score = evaluate(state, state.side_to_move());
            return is_claimable_draw_status(draw_status) ?
                std::max(0, static_score) : static_score;
        }
        // Qsearch can be entered before regular negamax initializes the frame
        // at this ply, and its recursive children enter the same way.  Reset
        // the frame before consulting the cache as well: a cache hit must not
        // leave a prior path's cutoff metadata for a later LMR decision.
        SearchFrame& qframe = stack.frame(static_cast<std::size_t>(std::max(ply, 0)));
        qframe.previous_move = previous_move.value_or(Move::no_move());
        qframe.in_check = checked;
        qframe.move_count = 0;
        qframe.reduction = 0;
        qframe.cutoff_count = 0;
        qframe.prior_fail_high = false;
        const bool use_qsearch_cache = qsearch_cache_allowed(state, repetition_sensitive) &&
            !(checked && qdepth >= kMaximumQuiescenceSafetyDepth);
        const std::uint64_t qsearch_key = use_qsearch_cache ?
            qsearch_cache_key(state, ply, qdepth, previous_move,
                              quiescence_check_depth_limit) : 0;
        if (use_qsearch_cache) {
            const QSearchCacheEntry& entry = *qsearch_cache_entry(qsearch_key);
            if (entry.valid && entry.key == qsearch_key &&
                (!entry.lower_bound || entry.score >= beta)) {
                ++stats.qsearch_cache_hits;
                if (repetition_sensitive_path != nullptr) {
                    // Only history-independent results are stored, so a hit
                    // never carries a repeated descendant into its caller.
                    *repetition_sensitive_path = false;
                }
                // A cached lower bound is usable for a matching fail-high
                // window, but it is not an exact qsearch result for the
                // nominal regular-search parent.
                path_selective_bound = entry.lower_bound;
                path_lower_bound = entry.lower_bound;
                return entry.score;
            }
        }
        // A checked qsearch node has no stand-pat value.  At the native
        // recursion safety boundary, use the heap-backed public move list only
        // to distinguish mate from a position with legal evasions, then stop
        // without constructing another large fixed metadata buffer or making
        // another recursive call.  This keeps the safety check ahead of both
        // the qsearch move list and TT bookkeeping.
        if (checked && qdepth >= kMaximumQuiescenceSafetyDepth) {
            const std::vector<Move> legal_moves = state.legal_moves();
            // Checkmate takes precedence over automatic draw clocks, but a
            // checked position with a legal evasion is still a draw when the
            // fivefold/75-move/dead-position rule has already ended the
            // game.  Keep the safety boundary's terminal semantics aligned
            // with the ordinary qsearch path without constructing another
            // metadata list.
            if (legal_moves.empty()) {
                return terminal_score(state, 0, ply);
            }
            const DrawStatus draw_status = state.draw_status();
            if (is_forced_draw_status(draw_status)) {
                return 0;
            }
            // A checked qsearch node with legal evasions has no stand-pat
            // fallback. Returning a neutral value at this hard safety
            // horizon is therefore a bounded frontier result, not a proof
            // at the regular node's nominal depth.
            path_selective_bound = true;
            return terminal_score(state, legal_moves.size(), ply);
        }

        const std::uint64_t qsearch_transposition_key = search_transposition_key(state);
        std::optional<TranspositionEntry> tt_entry;
        if (const auto entry = table_access.probe(qsearch_transposition_key, ply);
            entry.has_value()) {
            tt_entry = entry;
            ++stats.tt_hits;
        }
        MoveMetadataList moves;
        const bool has_legal_move = checked ?
            (state.legal_moves_with_metadata(moves, true, false), !moves.empty()) :
            state.legal_tactical_moves_with_metadata(
                moves, qdepth < quiescence_check_depth_limit, false);
        if (!has_legal_move) {
            const int score = terminal_score(state, 0, ply);
            if (use_qsearch_cache) {
                store_qsearch_cache(qsearch_key, score, false);
            }
            return score;
        }
        const DrawStatus draw_status = state.draw_status();
        if (is_forced_draw_status(draw_status)) {
            return 0;
        }
        const bool claimable_draw = is_claimable_draw_status(draw_status);

        // A depth-valid TT entry can short-circuit the tactical frontier.  A
        // qsearch entry is stored at depth zero, and any deeper regular entry
        // is at least as strong for this horizon.  Exact values are
        // authoritative; a proven lower bound may cut a fail-high.  Claimable
        // draws and repetition-sensitive paths stay on the full search
        // because their value depends on the rule state that produced it, and
        // a wide (PV-like) window does not accept a bound cutoff.
        if (!checked && !claimable_draw && !repetition_sensitive && tt_entry.has_value()) {
            const TranspositionEntry& entry = *tt_entry;
            if (entry.bound == TranspositionBound::exact) {
                return entry.score;
            }
            if (beta - alpha <= 1 && entry.bound == TranspositionBound::lower &&
                entry.score >= beta) {
                path_selective_bound = true;
                path_lower_bound = true;
                return entry.score;
            }
        }

        // At this boundary the tactical generator deliberately stops probing
        // quiet checks.  Even when no capture remains, the stand-pat value is
        // therefore only a bounded lower estimate rather than a complete
        // qsearch frontier.  Preserve that provenance before the empty-list
        // cache path below; terminal and forced-draw returns above remain exact.
        const bool quiet_check_horizon_reached = !checked &&
            qdepth >= quiescence_check_depth_limit;
        if (quiet_check_horizon_reached) {
            path_selective_bound = true;
        }

        // Qsearch move generation and pruning depend on qdepth and on the
        // move that led into the node. The shared TT key contains the board
        // and rule-clock identity but not these qsearch-only dimensions, so
        // the worker-local cache below includes them.

        // The ordinary tactical generator intentionally stops probing quiet
        // checks after the shallow horizon. At a later qsearch ply, inspect a
        // bounded quiet-check candidate set and retain only checks with a
        // narrow evasion set.
        // This rescues short mating nets without paying the full quiet-check
        // annotation cost at every quiescence node.
        if (!checked && qdepth >= kNarrowQuietCheckProbeStartDepth &&
            qdepth < kMaximumQuiescenceNarrowQuietCheckDepth) {
            MoveMetadataList quiet_check_candidates;
            state.legal_moves_with_metadata(
                quiet_check_candidates, true, false, CheckFlagMode::quiet_moves_only);
            int quiet_checks_added = 0;
            for (const MoveMetadata& candidate : quiet_check_candidates) {
                if (candidate.is_capture() || !candidate.gives_check ||
                    !narrow_deep_quiet_check_candidate(state, candidate)) {
                    continue;
                }
                (void)moves.push_back(candidate);
                if (++quiet_checks_added >= kMaximumQuiescenceNarrowQuietChecks) {
                    break;
                }
            }
        }

        const SearchHistoryContext history = history_context(ply, previous_move,
                                                               state.pawn_key());
        std::optional<Move> tt_move;
        if (tt_entry.has_value() && !tt_entry->best_move.is_no_move()) {
            tt_move = tt_entry->best_move;
        }

        int best = claimable_draw ? 0 : -kInfinity;
        const int original_alpha = alpha;
        bool best_lower_bound = false;
        bool unknown_selective_child = false;
        int selective_upper_bound = -kInfinity;
        Move best_move = Move::no_move();
        int qsearch_eval = kNoEvaluation;
        if (!checked) {
            // Reuse the unadjusted evaluation from a transposition hit when
            // one is available; the stored eval is perspective-raw exactly
            // like the evaluator's own return value.
            const int raw_eval = (tt_entry.has_value() && tt_entry->eval != kNoEvaluation) ?
                tt_entry->eval : evaluate(state, state.side_to_move());
            qsearch_eval = raw_eval;
            best = claimable_draw ? std::max(0, raw_eval) : raw_eval;
            // Stand-pat is a legal null continuation in qsearch terms, so it
            // provides a safe lower bound for the tactical frontier.  This
            // direction remains useful to a parent even when later pruning
            // makes the qsearch result selective.
            best_lower_bound = true;
            if (best >= beta) {
                path_selective_bound = true;
                path_lower_bound = true;
                if (use_qsearch_cache) {
                    store_qsearch_cache(qsearch_key, best, true);
                }
                if (!path_repetition_sensitive) {
                    // Stand-pat is a legal continuation, so it is a proven
                    // lower bound for the tactical frontier.
                    table_access.store(qsearch_transposition_key, 0, best,
                                       TranspositionBound::lower, Move::no_move(), ply, false,
                                       qsearch_eval);
                }
                return best;
            }
            alpha = std::max(alpha, best);
            if (qdepth >= kMaximumQuiescenceDepth) {
                if (!moves.empty()) {
                    path_selective_bound = true;
                }
                path_lower_bound = best_lower_bound;
                if (use_qsearch_cache) {
                    // The stand-pat score is a valid lower bound, but it is
                    // not exact when the tactical frontier still contains
                    // captures or checks that the safety horizon prevents us
                    // from searching.  Reusing it is safe only as a proven
                    // fail-high for a later window.
                    store_qsearch_cache(qsearch_key, best, true);
                }
                return best;
            }
            if (moves.empty()) {
                path_lower_bound = best_lower_bound;
                if (use_qsearch_cache) {
                    store_qsearch_cache(qsearch_key, best, quiet_check_horizon_reached);
                }
                return best;
            }
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
                // The bounded quiet-check continuation deliberately omits
                // this candidate at the edge of the qsearch check horizon.
                // Do not let the remaining stand-pat score masquerade as an
                // exact regular-search leaf.
                path_selective_bound = true;
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
                    const bool materially_interesting = metadata.see_score >= 0 ||
                        piece_value(metadata.captured_piece) >= piece_value(PieceType::rook);
                    if (move_count > kQuiescenceFutilityMoveLimit &&
                        !materially_interesting) {
                        path_selective_bound = true;
                        ++stats.delta_prunes;
                        continue;
                    }
                    const int futility_value =
                        futility_base + piece_value(metadata.captured_piece);
                    if (futility_value <= alpha) {
                        path_selective_bound = true;
                        if (futility_value > best) {
                            // This margin-based estimate is not a searched
                            // continuation, so it cannot remain the node's
                            // lower-bound certificate if it raises best.
                            best = futility_value;
                            best_lower_bound = false;
                        }
                        ++stats.delta_prunes;
                        continue;
                    }
                    if (capture && metadata.see_score < alpha - futility_base) {
                        path_selective_bound = true;
                        const int see_floor = std::min(alpha, futility_base);
                        if (see_floor > best) {
                            // The SEE floor is a pruning estimate rather than
                            // a proven child score; do not expose it as a
                            // lower bound after negation.
                            best = see_floor;
                            best_lower_bound = false;
                        }
                        ++stats.see_prunes;
                        continue;
                    }
                }

                // Quiet checks remain part of Koi's bounded qsearch
                // extension; ordinary quiet moves are not in the tactical
                // generator and are rejected defensively here.
                if (!capture && !metadata.gives_check && !promotion) {
                    path_selective_bound = true;
                    continue;
                }
                const QuiescenceCapturePrune capture_prune =
                    SearchPolicy::quiescence_capture(
                        checked, capture, metadata.gives_check, promotion,
                        metadata.see_score, piece_value(metadata.captured_piece), best, alpha);
                if (capture_prune == QuiescenceCapturePrune::static_exchange) {
                    path_selective_bound = true;
                    ++stats.see_prunes;
                    continue;
                }
                if (capture_prune == QuiescenceCapturePrune::delta) {
                    path_selective_bound = true;
                    ++stats.delta_prunes;
                    continue;
                }
            }
            if (metadata.gives_check &&
                (checked || qdepth < quiescence_check_depth_limit)) {
                ++stats.qchecks;
            }
            if (!make_observed(state, metadata, ply)) {
                // A current-position metadata record should always apply. If
                // the defensive make path rejects it, the generated frontier
                // was not searched completely and cannot be reported as an
                // exact qsearch result.
                path_selective_bound = true;
                continue;
            }
            bool child_repetition_sensitive = false;
            bool child_selective_bound = false;
            bool child_lower_bound = false;
            const int score = -quiescence(state, -beta, -alpha, ply + 1, qdepth + 1,
                                          metadata.move, &child_repetition_sensitive,
                                          &child_selective_bound, &child_lower_bound);
            unmake_observed(state, ply + 1);
            if (aborted) {
                return 0;
            }
            path_repetition_sensitive = path_repetition_sensitive ||
                child_repetition_sensitive;
            const bool child_score_is_selective_upper =
                child_selective_bound && child_lower_bound;
            if (child_selective_bound) {
                if (child_score_is_selective_upper) {
                    // The child proved a lower bound from its own side to
                    // move. Negation turns that into an upper bound for this
                    // qsearch node. Retain the strongest such challenger and
                    // decide whether it matters only after the final best
                    // lower bound is known.
                    selective_upper_bound = std::max(selective_upper_bound, score);
                } else {
                    // A selective result without a directional certificate
                    // may hide a stronger continuation even when its scout
                    // score failed low.
                    unknown_selective_child = true;
                }
            }
            // A known child lower bound becomes a safe upper bound after
            // negation. If it is already no better than the parent's current
            // best score, it cannot affect this node's result. An unknown
            // selective direction, however, may hide a stronger continuation
            // even when its returned score failed low, so it always taints the
            // parent provenance.
            path_selective_bound = path_selective_bound ||
                (child_selective_bound && !child_score_is_selective_upper);
            const bool child_score_lower_bound = !child_selective_bound &&
                !child_score_is_selective_upper && score > alpha;
            if (score > best) {
                best_lower_bound = child_score_lower_bound;
                best_move = metadata.move;
            } else if (score == best && child_score_lower_bound) {
                best_lower_bound = true;
            }
            best = std::max(best, score);
            // Tighten the window for every child so later siblings can still
            // produce a fail-high cutoff. Provenance is tracked separately
            // through path_selective_bound / best_lower_bound; freezing the
            // window for selective children suppresses pruning entirely and
            // makes a wide tactical frontier explode.
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                path_selective_bound = true;
                path_lower_bound = best_lower_bound;
                // A selective child result is a lower bound from the child's
                // point of view.  After negation it is an upper bound here,
                // so this cutoff is not proof that the current qsearch node
                // reaches beta.  Do not cache it as a lower bound; a later
                // probe could otherwise turn an unproven upper estimate into
                // a false fail-high.
                if (use_qsearch_cache && !path_repetition_sensitive &&
                    !child_selective_bound) {
                    store_qsearch_cache(qsearch_key, best, true);
                }
                if (best_lower_bound && !path_repetition_sensitive) {
                    // The best score is a proven floor from a non-selective
                    // child, so the shared table may keep it.
                    table_access.store(qsearch_transposition_key, 0, best,
                                       TranspositionBound::lower, best_move, ply, false,
                                       qsearch_eval);
                }
                break;
            }
        }
        // A non-cutoff qsearch result can still be an upper bound because a
        // child was searched through a null window. Keep the cache conservative:
        // only terminal/stand-pat values and proven lower cutoffs are stored.
        // This avoids turning a selective qsearch estimate into an exact TT
        // value while still making repeated fail-high frontiers cheap.
        // A selective child whose direction is known can be harmless once a
        // later candidate has a higher lower bound. Keep its upper bound until
        // the final best score is known instead of permanently tainting the
        // whole qsearch node merely because it was searched first.
        path_selective_bound = path_selective_bound ||
            unknown_selective_child ||
            (selective_upper_bound > -kInfinity &&
             (selective_upper_bound > best ||
              (selective_upper_bound == best && !best_lower_bound)));
        if (repetition_sensitive_path != nullptr) {
            *repetition_sensitive_path = path_repetition_sensitive;
        }
        path_lower_bound = best_lower_bound;
        if (!path_repetition_sensitive) {
            // Without a selective cutoff and with a raised alpha every
            // generated tactical candidate was searched, so the fail-soft
            // result is exact at this horizon.  A proven floor is still
            // useful when pruning touched the frontier, but it must stay a
            // lower bound.
            if (!path_selective_bound && best > original_alpha) {
                table_access.store(qsearch_transposition_key, 0, best,
                                   TranspositionBound::exact, best_move, ply, false,
                                   qsearch_eval);
            } else if (best_lower_bound) {
                table_access.store(qsearch_transposition_key, 0, best,
                                   TranspositionBound::lower, best_move, ply, false,
                                   qsearch_eval);
            }
        }
        return best;
    }

int SearchContext::negamax(GameState& state, int depth, int alpha, int beta, int ply,
                           PrincipalVariation& pv,
                           std::optional<Move> previous_move,
                           bool allow_null_pruning,
                           int check_extensions_remaining,
                           Move excluded_move,
                           bool* repetition_sensitive_path,
                           bool* selective_bound_path,
                           bool* root_authoritative_path,
                           bool* lower_bound_path) {
        bool path_repetition_sensitive = state.is_repetition_sensitive();
        const RepetitionPathGuard repetition_path_guard{
            repetition_sensitive_path, &path_repetition_sensitive};
        bool path_selective_bound = false;
        if (selective_bound_path != nullptr) {
            *selective_bound_path = false;
        }
        if (root_authoritative_path != nullptr) {
            *root_authoritative_path = false;
        }
        bool path_lower_bound = false;
        if (lower_bound_path != nullptr) {
            *lower_bound_path = false;
        }
        const LowerBoundPathGuard lower_bound_path_guard{
            lower_bound_path, &path_lower_bound};
        const SelectiveBoundPathGuard selective_bound_path_guard{
            selective_bound_path, &path_selective_bound};
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
            return quiescence(state, alpha, beta, ply, 0, previous_move,
                              &path_repetition_sensitive, &path_selective_bound,
                              &path_lower_bound);
        }
        if (ply >= static_cast<int>(SearchStack::kCapacity) - 2) {
            // The fixed stack guard is a last-resort horizon, not a license
            // to ignore terminal rules.  Let qsearch perform its compact
            // legal/terminal/draw handling for either side to move; a direct
            // static evaluation here would mis-score stalemate and a
            // rule-drawn position reached at the defensive boundary.
            const int score = quiescence(
                state, alpha, beta, ply, 0, previous_move,
                &path_repetition_sensitive, &path_selective_bound,
                &path_lower_bound);
            // A quiet nonterminal position can have an empty qsearch
            // frontier, in which case qsearch quite correctly returns its
            // stand-pat evaluation without setting a selective flag.  At
            // this regular-search stack boundary that value is still only a
            // safety horizon, not a nominal-depth proof. Terminal and
            // forced-draw positions remain authoritative; a claimable draw is
            // an option and therefore remains a nonterminal safety horizon.
            if (!state.is_terminal()) {
                path_selective_bound = true;
            }
            return score;
        }
        if (!count_node(false)) {
            return 0;
        }
        record_ply(ply);
        // This node's search key is stable for its whole lifetime, so compute
        // it once and let the cluster prefetch overlap the static evaluation,
        // pruning gates, and move ordering before the probe below.
        const std::uint64_t transposition_key = search_transposition_key(state);
        table_access.prefetch(transposition_key);

        // Quiescence performs its own terminal-aware tactical/evasion move
        // generation. Do not build and annotate the full legal move list here
        // only to discard it immediately at the depth boundary.
        const bool checked = state.in_check();
        const bool repetition_sensitive = path_repetition_sensitive;
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
        frame.had_tt_move = false;
        if (ply + 1 < static_cast<int>(SearchStack::kCapacity)) {
            // The current node reads the cutoff count owned by its immediate
            // child while deciding later-sibling LMR. Clear that slot before
            // the first candidate so an early make/prune failure cannot make
            // a stale cutoff from a prior path look like live feedback. A
            // searched child initializes the same slot and leaves its own
            // completed cutoff signal for subsequent siblings.
            stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count = 0;
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
        const DrawStatus draw_status = state.draw_status();
        const bool claimable_draw = is_claimable_draw_status(draw_status);
        // Automatic/dead draws are properties of the current position, not
        // of the remaining move subset. A claimable draw is different: it is
        // a legal zero-valued option for the side to move, so the move loop
        // must still be allowed to discover a better continuation.
        if (is_forced_draw_status(draw_status)) {
            return 0;
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
            return claimable_draw ? 0 :
                (excluded_search ? alpha : terminal_score(state, moves.size(), ply));
        }
        if (claimable_draw) {
            // A claim is an available floor, not a forced terminal result.
            // Raise the lower edge before node-type selection so a null
            // window with beta <= 0 can use the claim as a valid cutoff.
            alpha = std::max(alpha, 0);
            if (alpha >= beta) {
                path_selective_bound = true;
                path_lower_bound = true;
                return alpha;
            }
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
                // The tightened mate window avoided searching this node, so
                // the collapsed boundary is a bound rather than a
                // nominal-depth exact result for the parent.
                path_selective_bound = true;
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
        // The transposition probe runs before the static evaluation: a hit can
        // carry the unadjusted evaluation from an earlier visit, and an early
        // cutoff should not pay for evaluate().  This mirrors the reference
        // engine, where the TT lookup precedes both eval and razoring.  The
        // original window is captured first so a TT floor cannot change how
        // this node's bound is classified at the store site.
        const bool pv_node = beta - alpha > 1;
        const bool cut_node = !pv_node;
        const int original_alpha = alpha;
        const int original_beta = beta;
        const SearchHistoryContext history = history_context(
            ply, previous_move, state.pawn_key());
        std::optional<Move> tt_move;
        std::optional<TranspositionEntry> tt_entry;
        int tt_lower_bound_floor = -kInfinity;
        if (const auto entry = table_access.probe(transposition_key, ply);
            entry.has_value()) {
            tt_entry = entry;
            ++stats.tt_hits;
            if (!entry->best_move.is_no_move() && entry->best_move != excluded_move) {
                tt_move = entry->best_move;
            }
            if (!claimable_draw && !excluded_search && !repetition_sensitive && !pv_node && ply > 0 &&
                entry->depth >= depth) {
                if (entry->bound == TranspositionBound::exact) {
                    return entry->score;
                }
                if (entry->bound == TranspositionBound::lower) {
                    // Every depth-valid lower bound is a floor, including a
                    // bound that is already below the current alpha.  A later
                    // selective path must not contradict it merely because
                    // alpha was tightened before the floor was recorded.
                    tt_lower_bound_floor = entry->score;
                    alpha = std::max(alpha, entry->score);
                } else {
                    beta = std::min(beta, entry->score);
                }
                if (alpha >= beta) {
                    // A depth-valid quiet TT lower bound is a proven cut, but
                    // it bypasses the normal move loop and would otherwise
                    // never reinforce the worker-local ordering state.  Use
                    // a half-depth update so a stale/colliding entry cannot
                    // dominate a move that was searched at full depth.  The
                    // generated metadata check keeps this limited to a legal
                    // quiet move in the current position.
                    if (entry->bound == TranspositionBound::lower &&
                        !entry->best_move.is_no_move()) {
                        const auto tt_metadata = std::find_if(
                            moves.begin(), moves.end(), [&entry](const MoveMetadata& metadata) {
                                return metadata.move == entry->best_move;
                            });
                        if (tt_metadata != moves.end() && !tt_metadata->is_capture() &&
                            tt_metadata->move.promotion() == Promotion::none) {
                            const Color moving_side = history_side(state.side_to_move(), false);
                            ordering.record_quiet_cutoff(
                                history_side(moving_side, true), *tt_metadata,
                                std::max(1, depth / 2), history);
                            ++stats.quiet_history_updates;
                            if (history.count > 0) {
                                ++stats.continuation_history_updates;
                            }
                        }
                    }
                    // A bound-only TT cutoff is a valid window result, but
                    // it is not an exact nominal-depth score for the caller.
                    // Propagate that distinction so a parent PVS/root line
                    // re-searches before promoting the value to exact data.
                    path_selective_bound = true;
                    path_lower_bound = entry->bound == TranspositionBound::lower;
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
        // The child uses this to tell whether the first searched move here was
        // the transposition move, so refutation feedback only punishes an
        // early non-TT quiet move.
        frame.had_tt_move = tt_move.has_value();
        if (!excluded_search) {
            // PV provenance survives transposition; bound type alone cannot
            // recover it, so an entry marked PV keeps the stronger gates.
            frame.tt_pv = pv_node || (tt_entry.has_value() && tt_entry->pv);
        }

        int static_eval = 0;
        bool static_eval_valid = false;
        int raw_static_eval = kNoEvaluation;
        CorrectionKeys correction_keys{};
        bool correction_keys_valid = false;
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
            // The raw evaluator score is cached; the bounded correction is
            // applied on top of it so the cache stays perspective-raw.  A
            // transposition hit may already carry the unadjusted evaluation
            // from an earlier visit, in which case evaluate() is skipped.
            const int raw_eval = (tt_entry.has_value() && tt_entry->eval != kNoEvaluation) ?
                tt_entry->eval : evaluate(state, state.side_to_move());
            raw_static_eval = raw_eval;
            correction_keys = correction_keys_for(state);
            correction_keys_valid = true;
            static_eval = raw_eval + ordering.correction_value(
                correction_keys.pawn, correction_keys.material, correction_keys.king);
            static_eval_valid = true;
        }
        frame.static_eval = static_eval;
        frame.static_eval_valid = static_eval_valid;
        const SearchFrame& grandparent_frame = ply > 1 ?
            stack.frame(static_cast<std::size_t>(ply - 2)) : frame;
        bool improving = static_eval_valid && grandparent_frame.static_eval_valid &&
            static_eval > grandparent_frame.static_eval;
        const bool opponent_worsening = static_eval_valid && parent_frame.static_eval_valid &&
            static_eval > -parent_frame.static_eval;
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
        const bool phase_rich_quiet_position = !claimable_draw && !checked && depth == 1 &&
            !tactical_position &&
            ensure_features().game_phase >= 8;
        const PositionFeatures* quiet_forcing_parent_features = nullptr;
        const PositionFeatures* root_direct_forcing_features = nullptr;
        if (!checked && depth == 1 && ply == 1) {
            quiet_forcing_parent_features = &ensure_features();
        }
        if (!checked && depth == 1 && ply == 0 && allow_root_forcing_extension) {
            root_direct_forcing_features = &ensure_features();
        }
        if (phase_rich_quiet_position && !pv_node) {
            if (depth <= 1 && alpha > -kInfinity &&
                static_eval + kRazorMarginPerDepthSquared * depth * depth <= alpha) {
                bool razor_selective_bound = false;
                bool razor_lower_bound = false;
                const int razor_score = quiescence(state, alpha, beta, ply, 0,
                                                   previous_move,
                                                   &path_repetition_sensitive,
                                                   &razor_selective_bound,
                                                   &razor_lower_bound);
                if (!aborted && razor_score <= alpha) {
                    // Razoring replaces the nominal regular horizon with a
                    // qsearch upper-bound probe. It can safely prune this
                    // node, but its score must not be promoted through the
                    // recursive boundary as a full-depth child result.
                    path_selective_bound = true;
                    path_lower_bound = razor_lower_bound;
                    ++stats.razoring_prunes;
                    return razor_score;
                }
            }
        }

        // The transposition probe, the original-window capture, and the
        // PV/TT-move bookkeeping now run before the static evaluation so a
        // TT hit can supply the unadjusted eval; see the block above.

        // Interior Syzygy WDL probe.  Opt-in and off by default; the root probe
        // keeps its own gate.  Only quiet, non-repetition-sensitive interior
        // nodes at or below the configured remaining depth may cut off, and a
        // decisive result is a proven bound rather than a nominal-depth exact
        // score for this node.
        if (tablebase.interior_depth >= kMinimumSyzygyInteriorDepth && ply > 0 &&
            !excluded_search && !claimable_draw && !repetition_sensitive &&
            depth >= tablebase.interior_depth) {
            if (const std::optional<int> tablebase_score = probe_tablebase(state, depth);
                tablebase_score.has_value()) {
                ++stats.tbhits;
                path_selective_bound = true;
                path_lower_bound = *tablebase_score > 0;
                return *tablebase_score;
            }
        }

        // Null move is a cut-node probe.  Applying it at the root or on a PV
        // node lets a reduced, non-PV result masquerade as the authoritative
        // root score (especially during aspiration), and can also corrupt the
        // principal variation's first move.  Keep the compatibility policy
        // unchanged, but enforce the node-type boundary at its live call site.
        const bool null_move_shape_allowed = allow_null_pruning && !claimable_draw &&
            !excluded_search && ply >= null_verification_min_ply &&
            ply > 0 && cut_node;
        const bool null_move_allowed = null_move_shape_allowed && !repetition_sensitive;
        const NullMoveDecision null_move_gate = SearchPolicy::null_move(
            depth, alpha, beta, checked, null_move_shape_allowed);
        if (null_move_gate.eligible && repetition_sensitive) {
            ++stats.null_repetition_skips;
        }
        const bool pawn_endgame = !state.has_non_pawn_material(state.side_to_move()) ||
            !state.has_non_pawn_material(opposite(state.side_to_move()));
        const DynamicNullMoveDecision null_move_decision = SearchPolicy::dynamic_null_move(
            depth, alpha, beta, static_eval, checked,
            null_move_allowed,
            improving, pawn_endgame);
        if (null_move_decision.eligible && null_move_is_safe(state, ensure_features())) {
            if (make_null_observed(state, ply)) {
                PrincipalVariation null_pv;
                const int null_depth = std::max(
                    0, depth - null_move_decision.reduction);
                bool null_repetition_sensitive = false;
                bool null_selective_bound = false;
                const int null_score = -negamax(
                    state, null_depth, -beta, -beta + 1, ply + 1, null_pv,
                    std::optional<Move>{Move::no_move()}, false,
                    child_check_extensions_remaining, Move::no_move(),
                    &null_repetition_sensitive, &null_selective_bound);
                path_repetition_sensitive = path_repetition_sensitive ||
                    null_repetition_sensitive;
                unmake_null_observed(state, ply + 1);
                if (aborted) {
                    return 0;
                }
                if (null_score >= beta) {
                    int verified_score = null_score;
                    bool null_cutoff_proven = !null_selective_bound;
                    if (null_move_decision.verify) {
                        ++stats.null_verifications;
                        PrincipalVariation verification_pv;
                        const bool saved_table_state = table_access.enabled();
                        const int saved_null_verification_min_ply = null_verification_min_ply;
                        const SearchFrame saved_frame = frame;
                        const bool has_probe_child_frame =
                            ply + 1 < static_cast<int>(SearchStack::kCapacity);
                        const SearchFrame saved_probe_child_frame = has_probe_child_frame ?
                            stack.frame(static_cast<std::size_t>(ply + 1)) : SearchFrame{};
                        // The verification must stand on its own: a TT bound at
                        // this node or in its subtree is flagged as a selective
                        // result, which would make the confirmation unable to
                        // prove the cutoff.  The null branch already has its
                        // own key domain, so this disable is about evidence
                        // provenance rather than key collisions.
                        table_access.set_enabled(false);
                        null_verification_min_ply = ply + 3 * null_depth / 4;
                        try {
                            bool verification_repetition_sensitive = false;
                            bool verification_selective_bound = false;
                            verified_score = negamax(
                                state, null_depth, beta - 1, beta, ply, verification_pv,
                                previous_move, false, child_check_extensions_remaining,
                                Move::no_move(), &verification_repetition_sensitive,
                                &verification_selective_bound);
                            path_repetition_sensitive = path_repetition_sensitive ||
                                verification_repetition_sensitive;
                            null_cutoff_proven = !verification_selective_bound;
                        } catch (...) {
                            frame = saved_frame;
                            if (has_probe_child_frame) {
                                stack.frame(static_cast<std::size_t>(ply + 1)) =
                                    saved_probe_child_frame;
                            }
                            table_access.set_enabled(saved_table_state);
                            null_verification_min_ply = saved_null_verification_min_ply;
                            throw;
                        }
                        frame = saved_frame;
                        if (has_probe_child_frame) {
                            stack.frame(static_cast<std::size_t>(ply + 1)) =
                                saved_probe_child_frame;
                        }
                        table_access.set_enabled(saved_table_state);
                        null_verification_min_ply = saved_null_verification_min_ply;
                        if (aborted) {
                            return 0;
                        }
                    }
                    // A null probe may report a spurious score at its reduced
                    // horizon (for example after the side to move passes in
                    // a zugzwang-like checking position).  Once verification
                    // runs, only its score is a proven lower bound at the
                    // verification horizon; returning/storing the original
                    // null score could overstate that bound even when the
                    // verification still reaches beta.
                    const int cutoff_score = null_move_decision.verify ?
                        verified_score : null_score;
                    if (null_cutoff_proven && cutoff_score >= beta &&
                        cutoff_score < kMateThreshold) {
                        path_selective_bound = true;
                        // Verification removes the reduced-probe uncertainty,
                        // but null-move pruning is still a selective heuristic
                        // rather than a nominal-depth proof. Do not expose its
                        // score as a lower-bound direction: after negation a
                        // parent would otherwise treat it as a safe upper
                        // bound for an incompletely searched move.
                        path_lower_bound = false;
                        ++stats.null_cutoffs;
                        return cutoff_score;
                    }
                }
            }
        }

        // Stockfish 19 folds a static fail-high into the current-side
        // improving state after the null-move probe. That state feeds IIR,
        // ProbCut, and LMR; keeping the update after verification avoids
        // making the null gate itself more permissive. The separate
        // opponent-worsening signal remains reserved for hindsight depth
        // compensation, where it has a different meaning.
        improving = improving || (static_eval_valid && static_eval >= beta);

        // Reverse futility is useful at quiet scout nodes whose static score
        // is already comfortably above beta.  Keep the live envelope narrow:
        // no root/PV/check/tactical/excluded/repetition-sensitive or sparse
        // material position may turn this evaluation estimate into a cutoff.
        // The result is explicitly selective, so neither this node nor a
        // parent may promote it to nominal-depth exact information.
        const bool reverse_futility_allowed = !claimable_draw && !excluded_search && ply > 0 &&
            !repetition_sensitive && !state.is_repetition_sensitive() &&
            !pawn_endgame && !tactical_position && !tt_move.has_value() &&
            static_eval_valid;
        const ReverseFutilityDecision reverse_futility =
            SearchPolicy::reverse_futility(
                depth, beta, static_eval, checked, pv_node,
                reverse_futility_allowed, improving, opponent_worsening);
        if (reverse_futility.eligible) {
            path_selective_bound = true;
            // The static margin is a pruning estimate, not a proof that the
            // position reaches beta at this node's requested horizon. Its
            // direction is therefore unknown to a parent after negation.
            path_lower_bound = false;
            ++stats.reverse_futility_prunes;
            return static_eval - reverse_futility.margin;
        }

        // Internal iterative reduction is useful only when the node has no
        // move hint to establish a trustworthy search order. Keep the
        // reduction off PV nodes and tactical/repetition-sensitive positions:
        // those nodes are precisely where a missing TT move is least
        // informative and where a one-ply horizon change can hide a forcing
        // continuation. This is the conservative counterpart of Stockfish
        // 19's no-TT IIR gate.
        if (!claimable_draw && !excluded_search && ply > 0 && !pv_node && !checked &&
            !tactical_position && !state.is_repetition_sensitive() &&
            !tt_move.has_value() && depth >= kInternalIterativeReductionMinimumDepth) {
            path_selective_bound = true;
            --depth;
        }

        // ProbCut is deliberately confined to scout/cut nodes and to
        // positions with a reliable static margin. A capture must first hold
        // in qsearch and then hold in a reduced regular search; this mirrors
        // Stockfish 19's two-stage tactical verification and avoids turning a
        // speculative capture into a PV score.
        const ProbCutDecision probcut = SearchPolicy::prob_cut(
            depth, alpha, beta, static_eval, checked, improving,
            excluded_search, repetition_sensitive);
        if (!claimable_draw && probcut.eligible && cut_node && !repetition_sensitive &&
            !pawn_endgame &&
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
                if (!make_observed(state, candidate, ply)) {
                    continue;
                }
                PrincipalVariation probcut_pv;
                bool probcut_repetition_sensitive = false;
                bool probcut_qsearch_selective_bound = false;
                int probcut_score = -quiescence(
                    state, -probcut.beta, -probcut.beta + 1, ply + 1, 0,
                    candidate.move, &probcut_repetition_sensitive,
                    &probcut_qsearch_selective_bound);
                path_repetition_sensitive = path_repetition_sensitive ||
                    probcut_repetition_sensitive;
                if (!aborted && probcut_score >= probcut.beta && probcut.depth > 0) {
                    probcut_pv = {};
                    bool probcut_search_repetition_sensitive = false;
                    bool probcut_search_selective_bound = false;
                    probcut_score = -negamax(
                        state, probcut.depth, -probcut.beta, -probcut.beta + 1,
                        ply + 1, probcut_pv, candidate.move, false,
                        child_check_extensions_remaining, Move::no_move(),
                        &probcut_search_repetition_sensitive,
                        &probcut_search_selective_bound);
                    path_repetition_sensitive = path_repetition_sensitive ||
                        probcut_search_repetition_sensitive;
                    if (probcut_search_selective_bound) {
                        // A reduced ProbCut child that used razor, pruning,
                        // or another selective cutoff is only a scout. Its
                        // score cannot establish the lower bound that this
                        // ProbCut branch would otherwise publish.
                        probcut_score = -kInfinity;
                    }
                }
                unmake_observed(state, ply + 1);
                if (aborted) {
                    return 0;
                }
                // A depth-zero ProbCut has no reduced regular confirmation.
                // A qsearch lower bound is still useful as a probe, but it
                // cannot be the sole proof for the parent cutoff.
                if (probcut_score >= probcut.beta &&
                    (probcut.depth > 0 || !probcut_qsearch_selective_bound)) {
                    path_selective_bound = true;
                    // ProbCut deliberately proves only a reduced tactical
                    // probe. Keep it selective, but do not claim a lower
                    // bound at the parent horizon; otherwise negation can
                    // make an unsafe root upper-bound certificate.
                    path_lower_bound = false;
                    ++stats.probcut_cutoffs;
                    if (std::abs(probcut_score) < kMateThreshold) {
                        if (probcut.depth > 0 && !path_repetition_sensitive &&
                            table_access.enabled()) {
                            // The legal tactical child established a real
                            // lower bound at the reduced horizon. Retain it
                            // at that horizon for future move ordering and
                            // selective cutoffs, but never label it PV or
                            // store it at the parent's nominal depth.
                            table_access.store(
                                search_transposition_key(state), probcut.depth + 1,
                                probcut_score, TranspositionBound::lower,
                                candidate.move, ply, false, raw_static_eval);
                        }
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
        const bool tt_probcut = !claimable_draw && !excluded_search && !pv_node && !checked &&
            !pawn_endgame &&
            !state.is_repetition_sensitive() && tt_entry.has_value() &&
            tt_entry->bound == TranspositionBound::lower &&
            depth >= 4 && tt_entry->depth >= std::max(1, depth - 4) &&
            tt_entry->score >= beta + kTranspositionProbCutMargin &&
            std::abs(beta) < kMateThreshold &&
            std::abs(tt_entry->score) < kMateThreshold;
        if (tt_probcut) {
            path_selective_bound = true;
            // A TT lower bound is useful evidence for this selective probe,
            // but the soft margin still does not establish the current
            // nominal-depth node's lower-bound direction.
            path_lower_bound = false;
            return beta + kTranspositionProbCutMargin;
        }

        // True internal iterative deepening: when no transposition move is
        // available, re-search this node at depth - 2 with a null window so
        // the reduced search can seed the table with a move for the full-depth
        // ordering. The probe score is discarded, and the probe re-enters this
        // node's stack slot, so the frame fields the move loop still reads are
        // restored afterwards.
        const bool true_iid = !claimable_draw && !excluded_search && pv_node && !checked &&
            !tactical_position && !state.is_repetition_sensitive() && !tt_move.has_value() &&
            depth >= kTrueInternalIterativeDeepeningMinimumDepth;
        if (true_iid) {
            const int probe_depth = depth - 2;
            PrincipalVariation probe_pv;
            path_selective_bound = true;
            // The probe re-enters this node at the same ply, so it rewrites the
            // frame the move loop below reads; keep a copy and restore it.
            const SearchFrame saved_frame = frame;
            (void)negamax(state, probe_depth, alpha, alpha + 1, ply, probe_pv, previous_move,
                          false, check_extensions_remaining, Move::no_move(), nullptr, nullptr);
            if (aborted) {
                return 0;
            }
            frame = saved_frame;
            ++stats.internal_iterative_deepening;
            const std::optional<TranspositionEntry> probe_entry =
                table_access.probe(search_transposition_key(state), ply);
            if (probe_entry.has_value() && probe_entry->best_move != Move::no_move()) {
                tt_move = probe_entry->best_move;
            }
        }

        SearchMovePicker picker(
            ordering, state, moves, tt_move, history, ply,
            checked ? SearchMovePicker::Mode::evasion : SearchMovePicker::Mode::main,
            excluded_move);
        // A claimable draw is an implicit zero-valued option for the side to
        // move. Keep it as the initial floor while still searching every
        // legal move for a positive continuation.
        int best_score = claimable_draw ? 0 : -kInfinity;
        bool best_score_safe_lower_bound = claimable_draw;
        Move best_move = Move::no_move();
        int move_number = 0;
        bool prior_child_fail_high = false;
        bool singular_probe_done = false;
        // Only the first `failed_move_count` entries are ever read, so the
        // array is intentionally left uninitialized to avoid clearing ~3 KB
        // at every negamax node.
        std::array<DeferredHistoryMove, kMaximumLegalMoves> failed_moves;
        std::size_t failed_move_count = 0;
        MoveMetadata best_metadata{};
        Color best_history_side = side_to_move;
        bool best_metadata_valid = false;
        // A selected score is nominally authoritative only when the child
        // that established it actually searched the requested horizon without
        // a selective bound. Reduced children are re-searched before they can
        // challenge alpha; a negative singular extension is deliberately below
        // the nominal horizon and must not by itself create full-depth data.
        bool best_score_authoritative = false;
        // Keep lower-bound authority distinct from the nominal-depth/exact
        // status used for PV promotion. A full-depth cutoff remains useful
        // even when another sibling was only an inexact scout.
        bool best_score_lower_bound_authoritative = false;
        bool best_move_cutoff = false;
        bool selective_pruning = false;
        // A reduced child that failed low was not searched at the node's
        // authoritative horizon. It can still contribute to move ordering,
        // but its result must not justify an exact/upper TT entry at this
        // node. A later full-depth fail-high remains a valid lower bound.
        bool inexact_child_search = false;
        bool unknown_selective_child = false;
        int selective_upper_bound = -kInfinity;
        // An exact TT score is also a valid singularity reference.  Koi keeps
        // exact and lower bounds as separate enum values, whereas the
        // reference search represents an exact value as a bound containing
        // the lower bit.  Restrict this to PV nodes that reach the singular
        // path (non-PV exact hits return earlier), and retain the same depth,
        // mate, repetition, and material guards as lower-bound entries.
        const bool singular_tt_bound = tt_entry.has_value() &&
            (tt_entry->bound == TranspositionBound::lower ||
             tt_entry->bound == TranspositionBound::exact);
        const bool singular_candidate = !claimable_draw && !excluded_search && !checked &&
            !pawn_endgame && ply > 0 &&
            depth >= 6 + (frame.tt_pv ? 1 : 0) && !repetition_sensitive &&
            tt_move.has_value() && singular_tt_bound &&
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
            if (!claimable_draw && !excluded_search && ply > 0 && !pv_node && !checked &&
                !parent_frame.in_check &&
                parent_has_non_pawn_material &&
                best_score > -kMateThreshold && next_move_number >= quiet_skip_threshold) {
                picker.skip_quiet_moves();
                selective_pruning = true;
            }
            const Color moving_side = history_side(state.side_to_move(), false);
            const int history_score = metadata.is_capture() ? 0 :
                ordering.quiet_history_score(moving_side, metadata, history);
            const int continuation_score = ordering.continuation_history_score(metadata, history);
            const bool is_tt_move = tt_move.has_value() && move == *tt_move;
            const Move previous_history_move = history.count > 0 ?
                history.continuation_moves[0] : Move::no_move();
            const bool is_proven_counter_move = !previous_history_move.is_no_move() &&
                ordering.is_proven_counter_move(
                    moving_side, previous_history_move, move);
            const bool root_pawn_move = ply == 0 && metadata.moving_piece == PieceType::pawn;
            const int full_child_depth = depth - 1;
            const bool negative_continuation_allowed = !claimable_draw &&
                !excluded_search && ply > 0 && !repetition_sensitive &&
                !tactical_position && !pawn_endgame && !parent_frame.in_check &&
                !frame.tt_pv && best_score > -kMateThreshold;
            if (SearchPolicy::negative_continuation_history(
                    depth, next_move_number, continuation_score, pv_node, checked,
                    metadata.is_capture(), metadata.gives_check,
                    move.promotion() != Promotion::none, is_tt_move,
                    ordering.is_killer(move, ply), is_proven_counter_move,
                    negative_continuation_allowed)) {
                selective_pruning = true;
                ++stats.continuation_history_prunes;
                ++move_number;
                frame.move_count = move_number;
                continue;
            }
            const LateMoveDecision lmr_gate = excluded_search || claimable_draw ?
                LateMoveDecision{} :
                SearchPolicy::dynamic_late_move(
                    depth, next_move_number, full_child_depth, history_score,
                    continuation_score,
                    ordering.capture_history_score(metadata), root_pawn_move,
                    checked, metadata.gives_check, metadata.is_capture(),
                    move.promotion() != Promotion::none, is_tt_move,
                    ordering.is_killer(move, ply), false, false,
                    pv_node, cut_node, improving, prior_child_fail_high,
                    ply + 1 < static_cast<int>(SearchStack::kCapacity) ?
                        stack.frame(static_cast<std::size_t>(ply + 1)).cutoff_count : 0,
                    tt_move.has_value(), frame.tt_pv);
            if (lmr_gate.high_history_exclusion) {
                ++stats.lmr_high_history_exclusions;
            }
            // Depth-three reductions are useful in sparse/low-phase
            // verification positions, but opening positions still have a
            // wide, weakly learned quiet move set.  At that horizon a
            // reduction can turn a principled central break into a qsearch
            // estimate before its opponent's reply is visible.  Use the
            // existing Koi phase boundary to keep the shallow LMR available
            // where the low-phase regression needs it while requiring a full
            // first pass in phase-rich positions.
            const bool phase_rich_shallow_node = lmr_gate.candidate && depth == 3 &&
                !metadata.is_capture() && ensure_features().game_phase >= 8;
            // A checked root receives a one-ply root extension.  At the
            // resulting shallow horizon, serial search can carry history from
            // one evasion into the next while root workers cannot; avoid a
            // depth-three reduction there so threaded and serial evasions use
            // the same tactical horizon.  Ordinary quiet roots retain the
            // depth-three LMR needed by the verification path.
            const bool shallow_checked_root = ply == 1 && depth <= 3 &&
                stack.frame(0).in_check;
            const bool lmr_candidate = lmr_gate.candidate &&
                !lmr_gate.high_history_exclusion && !shallow_checked_root &&
                !phase_rich_shallow_node;
            if (lmr_candidate && !metadata.is_capture()) {
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
                bool excluded_repetition_sensitive = false;
                bool excluded_selective_bound = false;
                try {
                    excluded_score = negamax(
                        state, singular_depth, singular_beta - 1, singular_beta, ply,
                        singular_pv, previous_move, false,
                        child_check_extensions_remaining, move,
                        &excluded_repetition_sensitive, &excluded_selective_bound);
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
                path_repetition_sensitive = path_repetition_sensitive ||
                    excluded_repetition_sensitive;
                if (!excluded_selective_bound && excluded_score < singular_beta) {
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
                } else if (!excluded_selective_bound && !pv_node && depth >= 8 &&
                           excluded_score >= beta &&
                           excluded_score < kMateThreshold) {
                    path_selective_bound = true;
                    // Multi-cut is another selective probe: an alternative
                    // meeting beta does not prove the current node at its
                    // nominal horizon. Keep its direction unknown so a
                    // parent cannot convert it into a safe root upper bound.
                    path_lower_bound = false;
                    ++stats.multi_cut_prunes;
                    return excluded_score;
                } else if (!excluded_selective_bound && depth >= 8 &&
                           (tt_entry->score >= beta || cut_node)) {
                    // If the alternative moves are not singular, the TT move
                    // is still useful but need not receive full depth.
                    singular_extension = -3;
                }
            }
            if (!make_observed(state, metadata, ply)) {
                // A failed metadata application should be impossible for a
                // generator-owned candidate, but it still consumed one move
                // slot. Keep move-number-dependent pruning deterministic if a
                // defensive validation check rejects it.
                selective_pruning = true;
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            // Regular move generation intentionally omits capture check flags
            // to avoid probing the compatibility board for every tactical
            // candidate. Once the move is made, the native state is the
            // authoritative and cheap source of this fact. Preserve checking
            // captures from all post-make pruning gates even when their
            // metadata arrived unannotated.
            const bool child_in_check = state.in_check();
            bool king_zone_pressure = false;
            bool reducible_quiet = false;
            if (lmr_candidate) {
                if (metadata.is_capture()) {
                    // A non-checking capture has no quiet-move forcing
                    // features to inspect. Avoid paying for a feature rebuild
                    // on the tactical branch; its SEE/capture history already
                    // supplies the safety signal used by LMR.
                    reducible_quiet = !child_in_check;
                } else if (lmr_parent_features.has_value()) {
                    ++stats.position_feature_extractions;
                    const PositionFeatures after_quiet_features = state.position_features();
                    const std::size_t enemy = lmr_parent_features->side_to_move == Color::white ? 1U : 0U;
                    king_zone_pressure = after_quiet_features.king_zone_attacks[enemy] >
                        lmr_parent_features->king_zone_attacks[enemy];
                    reducible_quiet = !child_in_check &&
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
            const int child_extension_budget = child_check_extensions_remaining;
            const LateMoveDecision lmr_decision = excluded_search || claimable_draw ||
                !lmr_candidate ?
                LateMoveDecision{} :
                SearchPolicy::dynamic_late_move(
                    depth, next_move_number, full_child_depth, history_score,
                    continuation_score,
                    ordering.capture_history_score(metadata), root_pawn_move,
                    checked, metadata.gives_check, metadata.is_capture(),
                    move.promotion() != Promotion::none, is_tt_move,
                    ordering.is_killer(move, ply), reducible_quiet, quiet_forcing_extension,
                    pv_node, cut_node, improving, prior_child_fail_high,
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
            const bool child_depth_reduced_by_extension = extension_depth < 0;
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
            const bool capture_pruning = !claimable_draw && !excluded_search && ply > 0 &&
                !pv_node && !checked &&
                !child_in_check && !repetition_sensitive && move_number > 0 &&
                best_score > -kMateThreshold && parent_has_non_pawn_material &&
                metadata.is_capture() && !is_tt_move;
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
                    unmake_observed(state, ply + 1);
                    ++move_number;
                    frame.move_count = move_number;
                    continue;
                }
            }
            // Child futility is a narrow scout-node optimisation. It is
            // intentionally unavailable to the first move, PV nodes,
            // forcing moves, and TT moves, so a quiet move that is the only
            // plausible continuation still receives an authoritative search.
            // The reference envelope is keyed to the reduced depth, so late
            // reduced moves at deeper nodes are also covered; a static score
            // already above alpha reduces the margin because the position is
            // less clearly lost.
            const bool child_futility = !claimable_draw && !pv_node && !checked &&
                !tactical_position && move_number > 0 && !metadata.is_capture() &&
                !metadata.gives_check &&
                move.promotion() == Promotion::none && !is_tt_move &&
                lmr_depth < kChildFutilityMaximumLmrDepth &&
                static_eval + kChildFutilityBaseMargin +
                    kChildFutilityDepthMargin * lmr_depth +
                    (static_eval > alpha ? kChildFutilityEvalAboveAlphaMargin : 0) <= alpha;
            if (child_futility) {
                selective_pruning = true;
                ++stats.quiet_futility_prunes;
                unmake_observed(state, ply + 1);
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            if (!claimable_draw && !pv_node && SearchPolicy::quiet_futility(
                    phase_rich_quiet_position, metadata.is_capture(), metadata.gives_check,
                    move.promotion() != Promotion::none, move_number, static_eval, depth, alpha)) {
                selective_pruning = true;
                ++stats.quiet_futility_prunes;
                unmake_observed(state, ply + 1);
                ++move_number;
                frame.move_count = move_number;
                continue;
            }

            // Publish the reduction only while an actual child is about to
            // consume it. Pruned candidates never enter negamax/qsearch and
            // therefore must not leak their reduction into the next sibling.
            frame.reduction = reduced ? reduction : 0;
            if (child_depth_reduced_by_extension) {
                // A negative singular extension deliberately searches this
                // move below the node's nominal child depth. It remains a
                // useful ordering probe, but an exact/upper TT result for the
                // parent would overstate the depth unless every child was
                // searched authoritatively.
                inexact_child_search = true;
            }
            PrincipalVariation child_pv;
            int score = 0;
            bool reduced_child_verified = !reduced;
            bool exact_root_score = false;
            bool safe_upper_bound = false;
            bool child_repetition_sensitive = false;
            bool child_selective_bound = false;
            bool child_lower_bound = false;
            const bool root_forcing_move = allow_root_forcing_extension &&
                ply == 0 && quiet_forcing_extension;
            if (move_number == 0) {
                score = -negamax(
                    state, child_depth, -beta, -alpha, ply + 1, child_pv,
                    move, allow_null_pruning, child_extension_budget,
                    Move::no_move(), &child_repetition_sensitive,
                    &child_selective_bound, nullptr, &child_lower_bound);
                exact_root_score = (ply != 0 || (score > alpha && score < beta)) &&
                    !child_selective_bound;
            } else if (root_forcing_move) {
                score = -negamax(
                    state, child_depth, -kInfinity, kInfinity, ply + 1, child_pv,
                    move, allow_null_pruning, child_extension_budget,
                    Move::no_move(), &child_repetition_sensitive,
                    &child_selective_bound, nullptr, &child_lower_bound);
                exact_root_score = !child_selective_bound;
            } else {
                ++stats.pvs_searches;
                bool scout_selective_bound = false;
                score = -negamax(
                    state, child_depth, -alpha - 1, -alpha, ply + 1, child_pv,
                    move, allow_null_pruning, child_extension_budget,
                    Move::no_move(), &child_repetition_sensitive,
                    &scout_selective_bound, nullptr, &child_lower_bound);
                path_repetition_sensitive = path_repetition_sensitive ||
                    child_repetition_sensitive;
                child_selective_bound = scout_selective_bound;
                if (!aborted && reduced && score > alpha) {
                    ++stats.lmr_verifications;
                    child_pv = {};
                    bool verification_repetition_sensitive = false;
                    bool verification_selective_bound = false;
                    score = -negamax(
                        state, authoritative_child_depth, -beta, -alpha, ply + 1, child_pv,
                        move, allow_null_pruning, child_extension_budget,
                        Move::no_move(), &verification_repetition_sensitive,
                        &verification_selective_bound, nullptr, &child_lower_bound);
                    path_repetition_sensitive = path_repetition_sensitive ||
                        verification_repetition_sensitive;
                    child_selective_bound = verification_selective_bound;
                    reduced_child_verified = true;
                    exact_root_score = score > alpha && score < beta &&
                        !child_selective_bound;
                } else if (!aborted && !reduced && score > alpha && score < beta) {
                    ++stats.pvs_researches;
                    child_pv = {};
                    bool verification_repetition_sensitive = false;
                    bool verification_selective_bound = false;
                    score = -negamax(
                        state, authoritative_child_depth, -beta, -alpha, ply + 1, child_pv,
                        move, allow_null_pruning, child_extension_budget,
                        Move::no_move(), &verification_repetition_sensitive,
                        &verification_selective_bound, nullptr, &child_lower_bound);
                    path_repetition_sensitive = path_repetition_sensitive ||
                        verification_repetition_sensitive;
                    child_selective_bound = verification_selective_bound;
                    exact_root_score =
                        (ply != 0 || (score > alpha && score < beta)) &&
                        !child_selective_bound;
                }
            }
            // Any searched child can make the node's result depend on the
            // reversible ancestor set, even when that child is not the
            // current best move. Keep the provenance conservative across the
            // whole move loop so a later TT reuse cannot erase a
            // repetition-sensitive alternative from the path contract.
            path_repetition_sensitive = path_repetition_sensitive ||
                child_repetition_sensitive;
            unmake_observed(state, ply + 1);
            if (aborted) {
                return 0;
            }
            if (reduced && !reduced_child_verified) {
                inexact_child_search = true;
            }
            const bool child_nominal_depth_complete = !child_depth_reduced_by_extension &&
                (!reduced || reduced_child_verified);
            const bool child_selective_upper = child_nominal_depth_complete &&
                child_selective_bound && child_lower_bound;
            if (child_selective_bound) {
                if (child_selective_upper) {
                    // The child proved a lower bound from its own side to
                    // move. Negation turns it into an upper bound for this
                    // node. Keep the strongest challenger until the final
                    // lower bound is known; a later exact sibling can make
                    // this selective line harmless.
                    selective_upper_bound = std::max(selective_upper_bound, score);
                } else {
                    // A selective result with no trusted direction may hide
                    // a stronger move and must remain an unresolved source of
                    // inexactness for this node and its callers.
                    unknown_selective_child = true;
                    inexact_child_search = true;
                }
            }
            const bool child_score_nominally_authoritative =
                child_nominal_depth_complete && !child_selective_bound;
            const bool root_score_inexact = child_selective_bound ||
                !child_nominal_depth_complete;
            // A selective child is a safe upper bound for this candidate
            // after negation only when the child itself proved a lower bound.
            // The broad selective flag also covers unresolved/upper-bound
            // paths, so score-versus-alpha alone is not enough to certify
            // those lines. Ordinary non-selective fail-lows remain safe
            // upper bounds through the normal window contract.
            safe_upper_bound = !exact_root_score && child_nominal_depth_complete &&
                score <= alpha && (!child_selective_bound || child_lower_bound);
            const bool child_score_safe_lower_bound = child_nominal_depth_complete &&
                !child_selective_bound && score > alpha;
            const bool child_lower_bound_authoritative =
                child_score_nominally_authoritative && score >= original_beta;
            const bool child_raises_alpha = child_score_safe_lower_bound;
            prior_child_fail_high = child_raises_alpha;
            const bool quiet_history_move = !metadata.is_capture() &&
                move.promotion() == Promotion::none;
            const Color history_side_after_unmake = quiet_history_move ?
                history_side(moving_side, true) : moving_side;

            if (score > best_score) {
                if (best_metadata_valid && failed_move_count < failed_moves.size()) {
                    failed_moves[failed_move_count++] =
                        DeferredHistoryMove::from(
                            best_metadata, best_history_side, best_score_authoritative);
                }
                best_score = score;
                best_move = move;
                best_metadata = metadata;
                best_metadata_valid = true;
                best_history_side = history_side_after_unmake;
                best_score_authoritative = child_score_nominally_authoritative;
                best_score_lower_bound_authoritative = child_lower_bound_authoritative;
                best_score_safe_lower_bound = child_score_safe_lower_bound;
                pv.prepend(move, child_pv);
            } else {
                bool promoted_equal_authoritative = false;
                if (score == best_score && child_score_nominally_authoritative) {
                    // Preserve deterministic move ties when the incumbent is
                    // already authoritative.  If the incumbent came from an
                    // inexact/selective child, however, an equal full-depth
                    // child must become the selected move as well: otherwise
                    // the node would publish a proven score with an
                    // unproven PV/TT move.
                    if (!best_score_authoritative && best_metadata_valid) {
                        if (failed_move_count < failed_moves.size()) {
                            failed_moves[failed_move_count++] =
                                DeferredHistoryMove::from(
                                    best_metadata, best_history_side,
                                    best_score_authoritative);
                        }
                        best_move = move;
                        best_metadata = metadata;
                        best_history_side = history_side_after_unmake;
                        best_metadata_valid = true;
                        pv.prepend(move, child_pv);
                        promoted_equal_authoritative = true;
                    }
                    best_score_authoritative = true;
                    best_score_lower_bound_authoritative =
                        best_score_lower_bound_authoritative ||
                        child_lower_bound_authoritative;
                    best_score_safe_lower_bound = best_score_safe_lower_bound ||
                        child_score_safe_lower_bound;
                }
                if (!promoted_equal_authoritative && failed_move_count < failed_moves.size()) {
                    failed_moves[failed_move_count++] =
                        DeferredHistoryMove::from(
                            metadata, history_side_after_unmake,
                            child_score_nominally_authoritative);
                }
            }
            if (ply == 0 && root_move_score_count < root_move_scores.size()) {
                root_move_scores[root_move_score_count++] =
                    RootMoveScore{move, score, exact_root_score && child_nominal_depth_complete,
                                  root_score_inexact, safe_upper_bound,
                                  child_selective_upper};
            }
            // Advance the PVS window for every child so later siblings are
            // searched with a narrow window, but only allow a fail-high cutoff
            // when the incumbent is a proven lower bound. Selective scores are
            // useful for ordering and window narrowing yet must not terminate
            // the node on an unproven result.
            if (score > alpha) {
                alpha = score;
            }
            if (best_score_safe_lower_bound && alpha >= beta) {
                frame.cutoff_count++;
                frame.prior_fail_high = true;
                best_move_cutoff = true;
                break;
            }
            ++move_number;
            frame.move_count = move_number;
        }

        if (ply == 0 && claimable_draw && !best_metadata_valid &&
            root_move_score_count != 0) {
            // The claim option can be the root score even when every legal
            // move is below zero. Keep the UCI contract of returning a legal
            // move and choose the least-losing searched continuation as its
            // PV placeholder; the reported score remains the exact claim
            // value of zero.
            const RootMoveScore* fallback = &root_move_scores[0];
            for (std::size_t index = 1; index < root_move_score_count; ++index) {
                if (root_move_scores[index].score > fallback->score) {
                    fallback = &root_move_scores[index];
                }
            }
            const auto metadata = std::find_if(
                moves.begin(), moves.end(), [&fallback](const MoveMetadata& candidate) {
                    return candidate.move == fallback->move;
                });
            if (metadata != moves.end()) {
                best_move = fallback->move;
                best_metadata = *metadata;
                best_metadata_valid = true;
                best_history_side = history_side(side_to_move, false);
                pv.clear();
                pv.prepend(best_move, PrincipalVariation{});
            }
        }

        // Stockfish updates the selected move and all searched alternatives
        // after the node is known to be complete. Deferring the updates is
        // important here: a move that was temporarily best must receive a
        // failure malus if a later move overtakes it, rather than retaining a
        // positive best-move update from the same node.
        // A best move that came only from a reduced/selective child is not a
        // reliable ordering sample: its score may be an upper bound from the
        // reduced window, and skipped siblings were never compared against it
        // at the nominal horizon.  Keep all history feedback tied to a move
        // whose own child reached that horizon.  A verified fail-high remains
        // authoritative even when the cut node used quiet/SEE pruning, since
        // the cutoff itself is a valid lower-bound observation.
        const bool complete_history_comparison = !selective_pruning &&
            !inexact_child_search;
        const bool best_history_feedback =
            (best_move_cutoff && best_score_lower_bound_authoritative) ||
            (!best_move_cutoff && best_score_authoritative && complete_history_comparison);
        if (ply > 0 && best_metadata_valid && best_history_feedback) {
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
        }
        // A failure malus is comparative evidence, not just a fact about the
        // move.  Once the node skipped siblings or relied on an unverified
        // reduced child, the searched alternatives were not compared at one
        // common horizon; do not train them as if they had failed a complete
        // move set.  Verified cutoffs above remain useful lower-bound samples.
        if (ply > 0 && complete_history_comparison) {
            for (std::size_t index = 0; index < failed_move_count; ++index) {
                const DeferredHistoryMove& failed = failed_moves[index];
                if (!failed.authoritative) {
                    continue;
                }
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

        // Stockfish-style parent-move feedback.  A node that fails low is
        // evidence that the quiet move leading here was good; a node that
        // finds a refutation says the parent's first non-TT quiet move was
        // bad.  Only complete comparisons contribute, and the parent capture
        // case is skipped because its captured piece is no longer visible.
        if (ply > 0 && !excluded_search && complete_history_comparison &&
            history.count > 0 && !history.continuation_moves[0].is_no_move()) {
            const Move parent_move = history.continuation_moves[0];
            const Piece parent_piece = state.piece_at(parent_move.to());
            const Color parent_side = opposite(side_to_move);
            if (parent_move.promotion() == Promotion::none && !parent_piece.empty() &&
                parent_piece.color == parent_side) {
                if (best_score <= original_alpha) {
                    ordering.record_parent_fail_low(parent_side, parent_move, parent_piece.type,
                                                    ply - 1, history, depth);
                    ++stats.quiet_history_updates;
                } else {
                    const SearchFrame& parent_frame =
                        stack.frame(static_cast<std::size_t>(ply - 1));
                    const int first_non_tt_move = parent_frame.had_tt_move ? 2 : 1;
                    if (parent_frame.move_count == first_non_tt_move) {
                        ordering.record_parent_refuted(parent_side, parent_move, parent_piece.type,
                                                       ply - 1, history, depth);
                        ++stats.quiet_history_updates;
                    }
                }
            }
        }

        if (best_score == -kInfinity) {
            best_score = static_eval_valid ? static_eval : 0;
        }
        // A depth-valid lower TT entry is a proven floor. If selective
        // pruning or a narrow child path later produces a smaller value, do
        // not let that contradiction turn the node into an exact or upper
        // result. Preserve the known floor and expose the inconsistency as
        // non-authoritative provenance; the legal PV remains the searched
        // move, while this path is barred from becoming a fresh TT proof.
        if (tt_lower_bound_floor > -kInfinity && best_score < tt_lower_bound_floor) {
            best_score = tt_lower_bound_floor;
            best_score_authoritative = false;
            best_score_lower_bound_authoritative = false;
            best_score_safe_lower_bound = true;
            inexact_child_search = true;
            path_selective_bound = true;
        }
        // A node that skipped candidates or relied on an inexact reduced
        // child can still return a useful bound, but that result is not a
        // nominal-depth proof for its caller. Propagate the same provenance
        // used by null/ProbCut/razor cutoffs so a parent cannot promote it to
        // exact or authoritative full-depth metadata.
        const bool unresolved_selective_child = unknown_selective_child ||
            (selective_upper_bound > -kInfinity &&
             (!best_score_safe_lower_bound || selective_upper_bound > best_score));
        path_selective_bound = path_selective_bound || selective_pruning ||
            inexact_child_search || unresolved_selective_child;
        path_lower_bound = best_score_safe_lower_bound;
        // Feed the bounded correction history with the residual the search
        // found beyond the corrected static evaluation.  Mate scores and
        // inherited (checked/excluded) evaluations are not samples, and the
        // update never touches the transposition table.
        if (correction_keys_valid && !claimable_draw && best_metadata_valid &&
            std::abs(best_score) < kMateThreshold) {
            const int scaled_depth = std::min(depth, 8);
            const int bonus = std::clamp((best_score - static_eval) * scaled_depth / 8, -48, 48);
            if (bonus != 0) {
                ordering.update_correction(correction_keys.pawn, correction_keys.material,
                                           correction_keys.king, bonus);
                ++stats.correction_history_updates;
            }
        }
        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= original_beta ? TranspositionBound::lower : TranspositionBound::exact;
        // An exact bound requires at least one authoritative child and no
        // candidate removed by selective pruning. A node that searched no
        // child must not turn its static fallback into an exact TT answer;
        // that value is only a heuristic estimate for the current node.
        const bool safe_to_store = best_metadata_valid &&
            ((bound == TranspositionBound::lower &&
              best_score_lower_bound_authoritative) ||
             (!selective_pruning && !inexact_child_search &&
              !unresolved_selective_child));
        if (!excluded_search && !claimable_draw && !path_repetition_sensitive && safe_to_store) {
            table_access.store(transposition_key, depth, best_score, bound, best_move, ply,
                               frame.tt_pv, raw_static_eval);
        }
        if (root_authoritative_path != nullptr && ply == 0 && !excluded_search) {
            const bool complete_root_coverage = root_move_score_count == moves.size();
            const auto root_score_is_safe = [](const RootMoveScore& root_score,
                                                    const int incumbent_score,
                                                    const bool incumbent_has_lower_bound) {
                if (root_score.exact || root_score.safe_upper_bound) {
                    return true;
                }
                // A selective child lower bound becomes an upper bound after
                // negation. Once an exact/lower-bound incumbent is known to
                // be at least as good, that root line cannot displace it.
                return root_score.selective_upper_bound && incumbent_has_lower_bound &&
                    root_score.score <= incumbent_score;
            };
            bool claim_option_authoritative = claimable_draw && best_score == 0 &&
                best_metadata_valid && complete_root_coverage;
            if (claim_option_authoritative) {
                // With a claim available, zero is exact only when every root
                // move has been searched and no line can prove a positive
                // score. Ordinary fail-lows are safe upper bounds here; a
                // selective fail-high remains an unresolved challenger.
                for (std::size_t index = 0; index < root_move_score_count; ++index) {
                    const RootMoveScore& root_score = root_move_scores[index];
                    if (root_score.score > 0 ||
                        !root_score_is_safe(root_score, 0, true)) {
                        claim_option_authoritative = false;
                        break;
                    }
                }
            }
            bool root_authoritative = claim_option_authoritative ||
                (best_metadata_valid && best_score_authoritative && complete_root_coverage);
            if (root_authoritative) {
                bool exact_incumbent = false;
                for (std::size_t index = 0; index < root_move_score_count; ++index) {
                    const RootMoveScore& root_score = root_move_scores[index];
                    if (root_score.move == best_move) {
                        exact_incumbent = root_score.exact;
                    }
                    if (!root_score_is_safe(root_score, best_score,
                                            best_score_safe_lower_bound)) {
                        root_authoritative = false;
                        break;
                    }
                }
                root_authoritative = root_authoritative &&
                    (exact_incumbent || claim_option_authoritative);
            }
            *root_authoritative_path = root_authoritative;
        }
        return best_score;
    }

} // namespace koi::detail
