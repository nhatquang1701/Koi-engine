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

[[nodiscard]] constexpr bool is_claimable_draw_status(
    const DrawStatus status) noexcept {
    return status == DrawStatus::claimable_threefold ||
        status == DrawStatus::claimable_fifty_move;
}

[[nodiscard]] constexpr bool is_forced_draw_status(
    const DrawStatus status) noexcept {
    return status == DrawStatus::dead_position ||
        status == DrawStatus::automatic_fivefold ||
        status == DrawStatus::automatic_seventy_five_move;
}

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
        bool authoritative = true;

        [[nodiscard]] static DeferredHistoryMove from(
            const MoveMetadata& metadata, const Color side = Color::white,
            const bool child_authoritative = true) noexcept {
            return DeferredHistoryMove{
                metadata.move, metadata.moving_piece, metadata.captured_piece,
                metadata.kind, side, child_authoritative};
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
        bool exact = false;
        // An inexact/selective child can still be a safe upper bound for the
        // root move when it failed low against the incumbent. Keep that
        // direction separate from `exact`: the serial root may ignore such a
        // move while an unresolved selective challenger must block authority.
        bool selective_bound = false;
        bool safe_upper_bound = false;
        bool selective_upper_bound = false;
    };

    struct RootVerification {
        int score = -kInfinity;
        PrincipalVariation pv;
        bool authoritative = false;
    };

    struct EvaluationCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
    };

    struct QSearchCacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
        bool lower_bound = false;
    };

    struct RepetitionPathGuard {
        bool* output = nullptr;
        const bool* value = nullptr;

        ~RepetitionPathGuard() noexcept {
            if (output != nullptr && value != nullptr) {
                *output = *value;
            }
        }
    };

    struct SelectiveBoundPathGuard {
        bool* output = nullptr;
        const bool* value = nullptr;

        ~SelectiveBoundPathGuard() noexcept {
            if (output != nullptr && value != nullptr) {
                *output = *value;
            }
        }
    };

    struct LowerBoundPathGuard {
        bool* output = nullptr;
        const bool* value = nullptr;

        ~LowerBoundPathGuard() noexcept {
            if (output != nullptr && value != nullptr) {
                *output = *value;
            }
        }
    };

    static constexpr std::size_t kEvaluationCacheSize = 32 * 1024;
    static constexpr std::size_t kQSearchCacheSize = 4 * 1024;
    // Upper bound on how many `interrupted()` calls may skip the wall-clock
    // read. Every call still inspects the atomic stop flags, so an external
    // stop is immediate; only a timer deadline can be observed a few hundred
    // microseconds late.
    static constexpr int kClockPollInterval = 256;

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
    int clock_poll_countdown = 0;
    int quiescence_check_depth_limit = kMaximumQuiescenceCheckDepth;
    SearchStack stack;
    std::unique_ptr<EvaluationCacheEntry[]> evaluation_cache;
    std::array<QSearchCacheEntry, kQSearchCacheSize> qsearch_cache{};
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
            *budget <= kShortTimedSerialThreshold) {
            // A short timed search must finish its first root iteration before
            // spending time on the deepest quiet-checking layer. Fixed-depth
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

    [[nodiscard]] bool root_result_complete(const PrincipalVariation& pv) const noexcept {
        if (aborted || root_moves == nullptr || pv.length == 0 ||
            root_move_score_count != root_moves->size()) {
            return false;
        }
        // Every root score is recorded only after its child has returned. A
        // complete score set plus a PV whose first move belongs to that set
        // establishes coverage; callers may then apply the provenance rule
        // appropriate to publication versus strict root authority.
        return std::any_of(
            root_moves->begin(), root_moves->end(), [&pv](const MoveMetadata& metadata) {
                return metadata.move == pv.moves[0];
            });
    }

    [[nodiscard]] bool root_result_publishable(const PrincipalVariation& pv) const noexcept {
        if (!root_result_complete(pv)) {
            return false;
        }

        bool has_exact_incumbent = false;
        int exact_incumbent_score = -kInfinity;
        for (std::size_t index = 0; index < root_move_score_count; ++index) {
            const RootMoveScore& root_score = root_move_scores[index];
            if (root_score.exact) {
                has_exact_incumbent = true;
                exact_incumbent_score = std::max(exact_incumbent_score, root_score.score);
            }
        }

        // A fully covered root pass is useful heuristic progress when every
        // line is selective. Once an exact incumbent exists, an unresolved
        // selective challenger must not be allowed to disappear behind it.
        return std::all_of(
            root_move_scores.begin(), root_move_scores.begin() + root_move_score_count,
            [has_exact_incumbent, exact_incumbent_score](const RootMoveScore& root_score) {
                if (!has_exact_incumbent) {
                    return true;
                }
                return root_score.exact || root_score.safe_upper_bound ||
                    (root_score.selective_bound && root_score.selective_upper_bound &&
                     root_score.score <= exact_incumbent_score);
            });
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
        const auto is_losing_capture = [&metadata_for, &root](const Move move) {
            const MoveMetadata* metadata = metadata_for(move);
            return metadata != nullptr && metadata->is_capture() && metadata->see_computed &&
                metadata->see_score < 0 && !metadata->gives_check &&
                !root.move_gives_check(move);
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
            if (!candidate.exact) {
                continue;
            }
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
        bool selective_bound = false;
        try {
            verification.score = negamax(root, depth, -kInfinity, kInfinity, 0,
                                         verification.pv, std::nullopt, true,
                                         kMaximumCheckExtensionsPerPath, Move::no_move(),
                                         nullptr, &selective_bound);
            verification.authoritative = !selective_bound && !aborted;
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
        if (clock_poll_countdown > 0) {
            --clock_poll_countdown;
            return false;
        }
        clock_poll_countdown = kClockPollInterval;
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

    [[nodiscard]] static std::uint64_t qsearch_move_key(const Move move) noexcept {
        if (move.is_no_move()) {
            return 0;
        }
        return 1ULL + static_cast<std::uint64_t>(move.from().index()) +
            (1ULL + static_cast<std::uint64_t>(move.to().index()) << 7U) +
            (1ULL + static_cast<std::uint64_t>(move.promotion()) << 14U);
    }

    [[nodiscard]] static std::uint64_t qsearch_cache_key(
        const GameState& state, const int ply, const int qdepth,
        const std::optional<Move> previous_move, const int check_depth_limit) noexcept {
        // The regular TT key intentionally excludes qsearch-only context. A
        // private cache can include it without changing the public TT format;
        // mix every dimension before direct-mapped indexing so common move
        // pairs do not cluster in the small worker-local table.
        std::uint64_t key = state.position_key();
        key ^= (qsearch_move_key(previous_move.value_or(Move::no_move())) +
                0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, ply)) + 1ULL) *
            0x94D049BB133111EBULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, qdepth)) + 1ULL) *
            0xD6E8FEB86659FD93ULL;
        key ^= (static_cast<std::uint64_t>(std::max(0, check_depth_limit)) + 1ULL) *
            0xA24BAED4963EE407ULL;
        key ^= (static_cast<std::uint64_t>(state.halfmove_clock()) + 1ULL) *
            0x369DEA0F31A53F85ULL;
        key ^= key >> 30U;
        key *= 0xBF58476D1CE4E5B9ULL;
        key ^= key >> 27U;
        key *= 0x94D049BB133111EBULL;
        return key ^ (key >> 31U);
    }

    [[nodiscard]] QSearchCacheEntry* qsearch_cache_entry(
        const std::uint64_t key) noexcept {
        return &qsearch_cache[static_cast<std::size_t>(key) & (kQSearchCacheSize - 1)];
    }

    [[nodiscard]] const QSearchCacheEntry* qsearch_cache_entry(
        const std::uint64_t key) const noexcept {
        return &qsearch_cache[static_cast<std::size_t>(key) & (kQSearchCacheSize - 1)];
    }

    void store_qsearch_cache(const std::uint64_t key, const int score,
                             const bool lower_bound) noexcept {
        QSearchCacheEntry* entry = qsearch_cache_entry(key);
        // An exact result is stronger than a later lower-bound cutoff from a
        // different window. Do not downgrade it within this search context.
        if (entry->valid && entry->key == key && !entry->lower_bound && lower_bound) {
            return;
        }
        *entry = QSearchCacheEntry{key, score, true, lower_bound};
    }

    [[nodiscard]] bool qsearch_cache_allowed(
        const GameState& state, const bool repetition_sensitive) const noexcept {
        // Position keys do not encode the set of reversible ancestor keys.
        // A non-repeated position with a non-zero halfmove clock can therefore
        // still have a different repetition-sensitive descendant when reached
        // through another transposition.  Only a halfmove-zero boundary is
        // history-independent: the preceding pawn move/capture cannot be
        // undone by a future reversible qsearch continuation.
        return !repetition_sensitive && state.halfmove_clock() == 0;
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
                   std::optional<Move> previous_move = std::nullopt,
                   bool* repetition_sensitive_path = nullptr,
                   bool* selective_bound_path = nullptr,
                   bool* lower_bound_path = nullptr);

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv, std::optional<Move> previous_move = std::nullopt,
                bool allow_null_pruning = true,
                int check_extensions_remaining = kMaximumCheckExtensionsPerPath,
                Move excluded_move = Move::no_move(),
                bool* repetition_sensitive_path = nullptr,
                bool* selective_bound_path = nullptr,
                bool* root_authoritative_path = nullptr,
                bool* lower_bound_path = nullptr);
};

} // namespace koi::detail
