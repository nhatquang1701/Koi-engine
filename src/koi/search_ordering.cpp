#include "koi/detail/search_ordering.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "koi/detail/static_exchange.hpp"
#include "koi/piece_values.hpp"

namespace koi::detail {
namespace {

constexpr int kTtMovePriority = 1'000'000;
constexpr int kCapturePriority = 500'000;
constexpr int kPromotionPriority = 400'000;
constexpr int kCheckingMovePriority = 350'000;
constexpr int kKillerPriority = 300'000;
constexpr int kCounterMovePriority = kKillerPriority - 1;
constexpr int kSeeOrderingWeight = 12;
// Koi's history tables use a wider range than Stockfish's fixed-point quiet
// score. This threshold leaves initially neutral quiets in the good stage,
// while allowing repeatedly failing quiets to move behind bad captures.
constexpr int kGoodQuietPriorityThreshold = -4'096;
constexpr int kBadCapturePenalty = 210'000;
constexpr int kCaptureHistoryWeight = 2;

int piece_value(const PieceType type) noexcept {
    // Shared material table. A king can appear here as the *attacker* of a
    // capture delta, and Koi's ordering has always treated a king attacker as
    // weightless, so the king maps to 0 instead of kKingMaterialValue.
    // Kings can never be captured, so the victim side is unaffected.
    return type == PieceType::king ? 0 : piece_material_value(type);
}

int promotion_value(const Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::queen:
        return 900;
    case Promotion::rook:
        return 500;
    case Promotion::bishop:
        return 330;
    case Promotion::knight:
        return 320;
    case Promotion::none:
        return 0;
    }
    return 0;
}

// Ascending 64-bit key for the picker's per-stage sort.  The high half
// sign-flips and inverts the signed priority, so an ascending key orders
// priority descending; the low half carries the move tie-break key
// ascending.  source_index is deliberately not packed: move_tie_break_key is
// injective over the distinct legal moves of one candidate list, so the low
// half already separates every pair of records a stage range can contain
// (see ensure_stage_sorted).
[[nodiscard]] constexpr std::uint64_t picker_stage_sort_key(
    const int priority, const std::uint32_t tie_break) noexcept {
    return (static_cast<std::uint64_t>(
                ~(static_cast<std::uint32_t>(priority) ^ 0x80000000U)) << 32U) |
        tie_break;
}

} // namespace

std::uint32_t move_tie_break_key(const Move move) noexcept {
    const auto square_key = [](const Square square) noexcept -> std::uint32_t {
        if (square.index() == Square::kInvalid) {
            return 64;
        }
        return static_cast<std::uint32_t>(square.index() % 8) * 8U + square.index() / 8U;
    };

    std::uint32_t promotion_key = 0;
    switch (move.promotion()) {
    case Promotion::bishop:
        promotion_key = 1;
        break;
    case Promotion::knight:
        promotion_key = 2;
        break;
    case Promotion::queen:
        promotion_key = 3;
        break;
    case Promotion::rook:
        promotion_key = 4;
        break;
    case Promotion::none:
        break;
    }

    return (square_key(move.from()) * 64U + square_key(move.to())) * 5U + promotion_key;
}

void SearchMoveOrdering::clear() noexcept {
    tables_.clear();
    scored_move_count_ = 0;
}

int SearchMoveOrdering::correction_value(const std::uint64_t pawn_key,
                                         const std::uint64_t material_key,
                                         const std::uint64_t king_key) const noexcept {
    return tables_.correction_value(pawn_key, material_key, king_key);
}

void SearchMoveOrdering::update_correction(const std::uint64_t pawn_key,
                                           const std::uint64_t material_key,
                                           const std::uint64_t king_key,
                                           const int bonus) noexcept {
    tables_.update_correction(pawn_key, material_key, king_key, bonus);
}

bool SearchMoveOrdering::is_killer(const Move move, const int ply) const noexcept {
    return tables_.is_killer(move, ply);
}

void SearchMoveOrdering::order(const GameState& state, std::vector<Move>& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    std::vector<MoveMetadata> metadata;
    metadata.reserve(moves.size());
    for (const Move move : moves) {
        if (const auto described = state.describe_move(move); described.has_value()) {
            metadata.push_back(*described);
        }
    }
    order(state, metadata, tt_move, ply, previous_move);
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        moves[index] = metadata[index].move;
    }
}

void SearchMoveOrdering::order(const GameState& state, MoveMetadataList& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    scored_move_count_ = 0;
    // One exchange baseline serves every capture in this pass, mirroring
    // GameState::finalize_metadata; it is built at most once and only when a
    // capture still needs SEE.  Ordering never mutates the position, so the
    // baseline stays valid for the whole loop.
    std::optional<ExchangeContext> exchange_context;
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            if (!exchange_context.has_value()) {
                exchange_context = exchange_context_from(state);
            }
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(*exchange_context, metadata),
                static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            metadata.see_computed = true;
        }
        const int score = priority(state, metadata, tt_move, ply, previous_move);
        MoveMetadata scored_metadata = metadata;
        scored_metadata.ordering_score = score;
        scored_moves_[scored_move_count_++] = ScoredMove{
            scored_metadata, score, move_tie_break_key(metadata.move)};
    }

    std::sort(scored_moves_.begin(), scored_moves_.begin() + moves.size(),
              [](const ScoredMove& lhs, const ScoredMove& rhs) {
                  return lhs.priority != rhs.priority ? lhs.priority > rhs.priority :
                      lhs.tie_break < rhs.tie_break;
              });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves_[index].metadata;
    }
}

void SearchMoveOrdering::order(const GameState& state, MoveMetadataList& moves,
                               const std::optional<Move> tt_move,
                               const SearchHistoryContext& history_context) const {
    scored_move_count_ = 0;
    // Same shared baseline as the ply/previous-move overload above.
    std::optional<ExchangeContext> exchange_context;
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            if (!exchange_context.has_value()) {
                exchange_context = exchange_context_from(state);
            }
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(*exchange_context, metadata),
                static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            metadata.see_computed = true;
        }
        const int score = priority(state, metadata, tt_move, history_context);
        MoveMetadata scored_metadata = metadata;
        scored_metadata.ordering_score = score;
        scored_moves_[scored_move_count_++] = ScoredMove{
            scored_metadata, score, move_tie_break_key(metadata.move)};
    }

    std::sort(scored_moves_.begin(), scored_moves_.begin() + moves.size(),
              [](const ScoredMove& lhs, const ScoredMove& rhs) {
                  return lhs.priority != rhs.priority ? lhs.priority > rhs.priority :
                      lhs.tie_break < rhs.tie_break;
              });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves_[index].metadata;
    }
}

int SearchMoveOrdering::priority(const GameState& state, const MoveMetadata& metadata,
                                 const std::optional<Move> tt_move, const int ply,
                                 const std::optional<Move> previous_move) const {
    const Move move = metadata.move;
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }

    if (metadata.is_capture()) {
        const int victim_value = piece_value(metadata.captured_piece);
        const int attacker_value = piece_value(metadata.moving_piece);
        // Staged search scores captures before SEE is needed, just like
        // Stockfish 19's capture-history picker.  Once a candidate has been
        // materialized, the exact exchange result is a useful tie-breaker.
        const int see = metadata.see_computed ? std::clamp(
            static_cast<int>(metadata.see_score),
            -piece_value(PieceType::queen), piece_value(PieceType::queen)) : 0;
        const int capture_history = tables_.capture_history_score(metadata);
        const int bad_capture_penalty = metadata.see_computed && see < 0 && !metadata.gives_check ?
            kBadCapturePenalty : 0;
        return kCapturePriority - bad_capture_penalty + (victim_value * 16) - attacker_value +
            promotion_value(move.promotion()) + see * kSeeOrderingWeight +
            capture_history * kCaptureHistoryWeight;
    }
    if (move.promotion() != Promotion::none) {
        return kPromotionPriority + promotion_value(move.promotion());
    }
    if (metadata.gives_check) {
        const int check_history = quiet_history_score(
            state.side_to_move(), move, previous_move);
        return kCheckingMovePriority + std::clamp(check_history / 32, -8'192, 8'192);
    }

    const int killer_rank = tables_.killer_rank(move, ply);
    if (killer_rank == 2) {
        return kKillerPriority + 1;
    }
    if (killer_rank == 1) {
        return kKillerPriority;
    }
    if (previous_move.has_value() &&
        tables_.is_proven_counter_move(state.side_to_move(), *previous_move, move)) {
        return kCounterMovePriority;
    }
    return tables_.quiet_history_score(state.side_to_move(), move, previous_move);
}

int SearchMoveOrdering::priority(const GameState& state, const MoveMetadata& metadata,
                                 const std::optional<Move> tt_move,
                                 const SearchHistoryContext& history_context) const {
    const Move move = metadata.move;
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }

    if (metadata.is_capture()) {
        const int victim_value = piece_value(metadata.captured_piece);
        const int attacker_value = piece_value(metadata.moving_piece);
        const int see = metadata.see_computed ? std::clamp(
            static_cast<int>(metadata.see_score),
            -piece_value(PieceType::queen), piece_value(PieceType::queen)) : 0;
        const int capture_history = tables_.capture_history_score(metadata);
        const int bad_capture_penalty = metadata.see_computed && see < 0 && !metadata.gives_check ?
            kBadCapturePenalty : 0;
        return kCapturePriority - bad_capture_penalty + (victim_value * 16) - attacker_value +
            promotion_value(move.promotion()) + see * kSeeOrderingWeight +
            capture_history * kCaptureHistoryWeight;
    }
    if (move.promotion() != Promotion::none) {
        return kPromotionPriority + promotion_value(move.promotion());
    }
    if (metadata.gives_check) {
        const int check_history = tables_.quiet_history_score(
            state.side_to_move(), metadata, history_context);
        return kCheckingMovePriority + std::clamp(check_history / 32, -8'192, 8'192);
    }

    const int killer_rank = tables_.killer_rank(move, history_context.ply);
    if (killer_rank == 2) {
        return kKillerPriority + 1;
    }
    if (killer_rank == 1) {
        return kKillerPriority;
    }
    const Move previous = history_context.count > 0 ? history_context.continuation_moves[0] :
                                                        Move::no_move();
    if (!previous.is_no_move() && tables_.is_proven_counter_move(
            state.side_to_move(), previous, move)) {
        return kCounterMovePriority;
    }
    return tables_.quiet_history_score(state.side_to_move(), metadata, history_context);
}

void SearchMoveOrdering::order(const GameState& state, std::vector<MoveMetadata>& moves,
                               const std::optional<Move> tt_move, const int ply,
                               const std::optional<Move> previous_move) const {
    scored_move_count_ = 0;
    // Same shared baseline as the other ordering overloads.
    std::optional<ExchangeContext> exchange_context;
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            if (!exchange_context.has_value()) {
                exchange_context = exchange_context_from(state);
            }
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(*exchange_context, metadata),
                static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            metadata.see_computed = true;
        }
        const int score = priority(state, metadata, tt_move, ply, previous_move);
        MoveMetadata scored_metadata = metadata;
        scored_metadata.ordering_score = score;
        scored_moves_[scored_move_count_++] = ScoredMove{
            scored_metadata, score, move_tie_break_key(metadata.move)};
    }

    std::sort(scored_moves_.begin(), scored_moves_.begin() + moves.size(),
              [](const ScoredMove& lhs, const ScoredMove& rhs) {
                  return lhs.priority != rhs.priority ? lhs.priority > rhs.priority :
                      lhs.tie_break < rhs.tie_break;
              });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves_[index].metadata;
    }
}

int SearchMoveOrdering::quiet_history_score(
    const Color side, const Move move, const std::optional<Move> previous_move) const noexcept {
    return tables_.quiet_history_score(side, move, previous_move);
}

int SearchMoveOrdering::quiet_history_score(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& history_context) const noexcept {
    return tables_.quiet_history_score(side, metadata, history_context);
}

int SearchMoveOrdering::continuation_history_score(
    const MoveMetadata& metadata, const SearchHistoryContext& history_context) const noexcept {
    return tables_.continuation_history_score(metadata, history_context);
}

int SearchMoveOrdering::capture_history_score(
    const MoveMetadata& metadata) const noexcept {
    return tables_.capture_history_score(metadata);
}

bool SearchMoveOrdering::is_proven_counter_move(
    const Color side, const Move previous_move, const Move move) const noexcept {
    return tables_.is_proven_counter_move(side, previous_move, move);
}

void SearchMoveOrdering::record_quiet_cutoff(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    tables_.record_quiet_cutoff(side, move, ply, depth, previous_move);
}

void SearchMoveOrdering::record_quiet_fail(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    tables_.record_quiet_fail(side, move, ply, depth, previous_move);
}

void SearchMoveOrdering::record_quiet_cutoff(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_cutoff(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_quiet_best(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_best(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_quiet_fail(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_quiet_fail(side, metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_cutoff(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_cutoff(metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_best(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_best(metadata, depth, history_context);
}

void SearchMoveOrdering::record_capture_fail(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& history_context) noexcept {
    tables_.record_capture_fail(metadata, depth, history_context);
}

void SearchMoveOrdering::record_parent_fail_low(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& history_context, const int depth) noexcept {
    tables_.record_parent_fail_low(side, move, piece, parent_ply, history_context, depth);
}

void SearchMoveOrdering::record_parent_refuted(
    const Color side, const Move move, const PieceType piece, const int parent_ply,
    const SearchHistoryContext& history_context, const int depth) noexcept {
    tables_.record_parent_refuted(side, move, piece, parent_ply, history_context, depth);
}

SearchMovePicker::SearchMovePicker(
    const SearchMoveOrdering& ordering, const GameState& state,
    const MoveMetadataList& moves, std::optional<Move> tt_move,
    const SearchHistoryContext& history_context, const int ply, const Mode mode,
    const Move excluded_move) noexcept
    : ordering_(ordering), state_(state), moves_(moves),
      history_context_(history_context), tt_move_(tt_move),
      excluded_move_(excluded_move), ply_(ply), mode_(mode), stage_(first_stage()) {}

bool SearchMovePicker::is_excluded(const std::size_t index) const noexcept {
    return index >= moves_.size() || emitted_.test(index) ||
        moves_[index].move == excluded_move_;
}

bool SearchMovePicker::is_good_capture(const MoveMetadata& metadata) const noexcept {
    // A quiet promotion is still a forcing material transformation.  It has
    // no SEE score to certify it as a capture, so never defer it with poisoned
    // captures merely because it does not give check.
    if (metadata.move.promotion() != Promotion::none) {
        return true;
    }
    if (!metadata.is_capture()) {
        return false;
    }
    // Stockfish 19 scales the SEE threshold with capture history and victim
    // value (its `-cur->value / 18` gate). A fixed threshold over-searches
    // poorly ordered pawn captures while delaying high-value tactical
    // recaptures, so retain that calibrated relationship in centipawn units.
    const int capture_score = ordering_.capture_history_score(metadata) +
        7 * piece_value(metadata.captured_piece);
    const int threshold = std::clamp(-(capture_score / 18), -1024, 1024);
    return metadata.gives_check || metadata.see_score >= threshold;
}

SearchMovePicker::Stage SearchMovePicker::first_stage() const noexcept {
    return Stage::tt;
}

MoveMetadata SearchMovePicker::materialize(const std::size_t index) {
    MoveMetadata metadata = moves_[index];
    if (metadata.is_capture()) {
        if (!see_computed_.test(index)) {
            if (metadata.see_computed) {
                see_scores_[index] = metadata.see_score;
            } else {
                if (!see_context_.has_value()) {
                    see_context_ = exchange_context_from(state_);
                }
                see_scores_[index] = static_cast<std::int16_t>(std::clamp(
                    static_exchange_gain(*see_context_, metadata),
                    static_cast<int>(std::numeric_limits<std::int16_t>::min()),
                    static_cast<int>(std::numeric_limits<std::int16_t>::max())));
            }
            see_computed_.set(index);
        }
        metadata.see_score = see_scores_[index];
        metadata.see_computed = true;
    }
    const bool promotion = metadata.move.promotion() != Promotion::none;
    if (mode_ == Mode::main && (metadata.is_capture() || promotion) &&
        !metadata.gives_check && !capture_check_computed_.test(index)) {
        capture_checks_[index] = state_.move_gives_check(metadata.move);
        capture_check_computed_.set(index);
    }
    if (capture_check_computed_.test(index)) {
        metadata.gives_check = metadata.gives_check || capture_checks_.test(index);
    }
    return metadata;
}

void SearchMovePicker::prepare_candidates() {
    candidate_count_ = 0;
    candidate_index_ = 0;
    deferred_bad_capture_count_ = 0;
    deferred_bad_capture_index_ = 0;
    candidates_prepared_ = true;

    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (is_excluded(index)) {
            continue;
        }
        // Rank from the stored metadata directly; only the materialize path
        // needs a mutable copy.
        MoveMetadata materialized{};
        const MoveMetadata* metadata = &moves_[index];

        Stage candidate_stage = Stage::done;
        if (mode_ == Mode::evasion) {
            candidate_stage = Stage::evasions;
        } else if (mode_ == Mode::quiescence) {
            // Stockfish's qsearch capture stage emits the complete capture
            // list and lets qsearch SEE/delta pruning decide what is safe;
            // do not discard a mildly losing recapture in the picker.
            if (metadata->is_capture() || metadata->move.promotion() != Promotion::none) {
                candidate_stage = Stage::good_captures;
            } else if (metadata->gives_check) {
                candidate_stage = Stage::quiet_checks;
            }
        } else if (metadata->move.promotion() != Promotion::none || metadata->is_capture()) {
            candidate_stage = Stage::good_captures;
        } else {
            // Quiet checks, killers, counter moves, and ordinary quiets all
            // compete in one history-ranked quiet pass.  Their priority
            // carries the forcing/special-move bonus; the stage split only
            // separates the proven good quiet prefix from the tail that can
            // safely be searched after deferred captures.
            candidate_stage = Stage::good_quiets;
        }
        if (candidate_stage == Stage::done) {
            continue;
        }
        // Qsearch deliberately asks the generator for captures without SEE
        // so the picker can own the work.  Materialize tactical candidates
        // before ranking them, however: qsearch may discard candidates after
        // only its first two ordered moves, and a victim/attacker score alone
        // can put a sound defensive recapture behind a poisoned capture.
        // This remains fixed-storage and computes SEE only for candidates
        // entering a tactical stage.
        if (mode_ == Mode::quiescence &&
            (metadata->is_capture() || metadata->move.promotion() != Promotion::none)) {
            materialized = materialize(index);
            metadata = &materialized;
        }
        const int priority = ordering_.priority(
            state_, *metadata, std::nullopt, history_context_);
        if (mode_ == Mode::main && candidate_stage == Stage::good_quiets &&
            priority <= kGoodQuietPriorityThreshold) {
            candidate_stage = Stage::bad_quiets;
        }
        staged_[candidate_count_++] = Candidate{
            static_cast<std::uint16_t>(index), static_cast<std::uint8_t>(candidate_stage),
            priority, picker_stage_sort_key(priority, move_tie_break_key(metadata->move))};
    }

    // Partition by stage rank and sort each stage lazily.  The previous
    // whole-array comparator's first key was the stage rank itself, so
    // grouping by rank and then sorting each group with the remaining keys
    // produces exactly the same emission order; stages that pruning never
    // reaches are never sorted at all.
    //
    // The grouping itself is a counting partition rather than a comparison
    // sort.  The per-stage packed key below is a strict total order because
    // its tie-break half is unique per candidate (see ensure_stage_sorted),
    // so the order inside a stage is fixed by that sort alone and the
    // partition is free to be unstable.
    std::array<std::size_t, kStageRankCount> counts{};
    for (std::size_t i = 0; i < candidate_count_; ++i) {
        ++counts[staged_[i].stage_rank];
    }
    std::size_t stage_offset = 0;
    for (std::size_t rank = 0; rank < kStageRankCount; ++rank) {
        stage_begin_[rank] = static_cast<std::uint16_t>(stage_offset);
        stage_offset += counts[rank];
        stage_end_[rank] = static_cast<std::uint16_t>(stage_offset);
    }
    // In-place counting permutation: move each misplaced candidate to the
    // write cursor of its rank's range.  Every swap settles one slot, so the
    // scan performs at most candidate_count_ swaps.
    std::array<std::size_t, kStageRankCount> write{};
    for (std::size_t rank = 0; rank < kStageRankCount; ++rank) {
        write[rank] = stage_begin_[rank];
    }
    for (std::size_t i = 0; i < candidate_count_; ++i) {
        std::size_t rank = staged_[i].stage_rank;
        while (i < stage_begin_[rank] || i >= stage_end_[rank]) {
            std::swap(staged_[i], staged_[write[rank]]);
            ++write[rank];
            rank = staged_[i].stage_rank;
        }
    }
    stage_sorted_.reset();
}

void SearchMovePicker::ensure_stage_sorted() noexcept {
    const std::size_t rank = static_cast<std::size_t>(stage_);
    if (rank >= kStageRankCount || stage_sorted_.test(rank)) {
        return;
    }
    stage_sorted_.set(rank);
    // One ascending 64-bit key replaces the former three-key comparator
    // (priority descending, tie_break ascending, source_index ascending).
    // The third key was unreachable for distinct records: move_tie_break_key
    // maps (from, to, promotion) through the square-key permutation
    // (index % 8) * 8 + index / 8 and then (from_key * 64 + to_key) * 5 +
    // promotion_key, which is injective, and every candidate list handed to
    // the picker is built from the legal-move generator, which emits each
    // (from, to, promotion) triple at most once (each origin is visited once;
    // every step and ray target is distinct; a promotion enumerates four
    // distinct pieces; castling is the only two-square king move).  Filtering
    // and compaction only remove records, so two records of one candidate
    // list can never share a tie-break key.  Distinct records therefore
    // always differ in the packed key, and this sort's total order is
    // identical to the old comparator's.
    std::sort(staged_.begin() + stage_begin_[rank], staged_.begin() + stage_end_[rank],
              [](const Candidate& lhs, const Candidate& rhs) {
                  return lhs.sort_key < rhs.sort_key;
              });
}

void SearchMovePicker::advance_stage() noexcept {
    switch (stage_) {
    case Stage::tt:
        stage_ = mode_ == Mode::evasion ? Stage::evasions : Stage::good_captures;
        break;
    case Stage::good_captures:
        stage_ = mode_ == Mode::quiescence ? Stage::quiet_checks : Stage::good_quiets;
        break;
    case Stage::good_quiets:
        stage_ = Stage::bad_captures;
        break;
    case Stage::bad_captures:
        stage_ = mode_ == Mode::main ? Stage::bad_quiets : Stage::done;
        break;
    case Stage::bad_quiets:
    case Stage::quiet_checks:
        stage_ = Stage::done;
        break;
    case Stage::evasions:
    case Stage::done:
        stage_ = Stage::done;
        break;
    }
}

std::optional<MoveMetadata> SearchMovePicker::emit_tt() {
    if (tt_emitted_ || !tt_move_.has_value()) {
        return std::nullopt;
    }
    tt_emitted_ = true;
    for (std::size_t index = 0; index < moves_.size(); ++index) {
        if (!is_excluded(index) && moves_[index].move == *tt_move_) {
            emitted_.set(index);
            MoveMetadata metadata = mode_ == Mode::evasion ? moves_[index] :
                materialize(index);
            // The emitted move equals tt_move_, which priority() answers with
            // the TT constant before any other branch; materialize never
            // changes the move.
            metadata.ordering_score = kTtMovePriority;
            return metadata;
        }
    }
    return std::nullopt;
}

std::optional<MoveMetadata> SearchMovePicker::next() {
    for (;;) {
        if (stage_ == Stage::done) {
            return std::nullopt;
        }
        if (stage_ == Stage::tt) {
            if (const auto tt = emit_tt(); tt.has_value()) {
                advance_stage();
                return tt;
            }
            advance_stage();
            continue;
        }
        // Quiet checks, killers, and proven counters receive large priority
        // bonuses inside the good-quiet stage.  Once a cut node decides to
        // skip quiets, the remaining quiet prefix and tail are both deferred;
        // captures still flow through their own stages.
        if (skip_quiets_ && (stage_ == Stage::good_quiets ||
                             stage_ == Stage::bad_quiets)) {
            advance_stage();
            continue;
        }
        if (!candidates_prepared_) {
            prepare_candidates();
        }
        ensure_stage_sorted();
        while (candidate_index_ < candidate_count_ &&
               staged_[candidate_index_].stage_rank < static_cast<std::uint8_t>(stage_)) {
            ++candidate_index_;
        }
        if (candidate_index_ < candidate_count_ &&
            staged_[candidate_index_].stage_rank == static_cast<std::uint8_t>(stage_)) {
            const std::size_t candidate_slot = candidate_index_++;
            const Candidate candidate = staged_[candidate_slot];
            emitted_.set(candidate.source_index);
            MoveMetadata metadata = mode_ == Mode::evasion ? moves_[candidate.source_index] :
                materialize(candidate.source_index);
            if (mode_ == Mode::main && stage_ == Stage::good_captures &&
                metadata.is_capture() && !is_good_capture(metadata)) {
                if (deferred_bad_capture_count_ < deferred_bad_capture_indices_.size()) {
                    // Keep the staged slot rather than the source index so the
                    // candidate's staged priority is still reachable when the
                    // bad-capture stage emits the deferred record.
                    deferred_bad_capture_indices_[deferred_bad_capture_count_++] =
                        static_cast<std::uint16_t>(candidate_slot);
                }
                continue;
            }
            // The staged priority is reusable unless materializing the
            // metadata could still change what the priority depends on.  That
            // is exactly the main-mode capture case: materialize resolves the
            // lazy capture check flag and the SEE value, and the capture
            // priority branch reads both.  Promotions cannot enter that
            // branch: the generator classifies every promotion, including a
            // capturing one, as MoveKind::promotion, so is_capture() is false
            // and priority returns kPromotionPriority plus the promotion
            // value, which materialize never changes.  Evasion, quiescence,
            // and quiet-stage candidates were fully materialized before
            // ranking.
            const bool materialization_can_alter_priority =
                mode_ == Mode::main && metadata.is_capture();
            if (materialization_can_alter_priority &&
                !moves_[candidate.source_index].see_computed) {
                // Search lists defer SEE, so the staged priority ranked this
                // capture with see == 0 and no bad-capture penalty; the
                // materialized priority is exactly the staged priority plus
                // those two resolved terms.
                const int see = std::clamp(
                    static_cast<int>(metadata.see_score),
                    -piece_value(PieceType::queen), piece_value(PieceType::queen));
                const int bad_capture_penalty =
                    see < 0 && !metadata.gives_check ? kBadCapturePenalty : 0;
                metadata.ordering_score =
                    candidate.priority + see * kSeeOrderingWeight - bad_capture_penalty;
            } else {
                metadata.ordering_score = materialization_can_alter_priority
                    ? ordering_.priority(state_, metadata, std::nullopt, history_context_)
                    : candidate.priority;
            }
            return metadata;
        }
        if (stage_ == Stage::bad_captures) {
            if (deferred_bad_capture_index_ < deferred_bad_capture_count_) {
                const std::size_t candidate_slot =
                    deferred_bad_capture_indices_[deferred_bad_capture_index_++];
                const Candidate& candidate = staged_[candidate_slot];
                const std::size_t source_index = candidate.source_index;
                MoveMetadata metadata = materialize(source_index);
                if (!moves_[source_index].see_computed) {
                    // Same SEE delta as the good-capture stage: the staged
                    // priority ranked this capture with see == 0 and no
                    // bad-capture penalty.
                    const int see = std::clamp(
                        static_cast<int>(metadata.see_score),
                        -piece_value(PieceType::queen), piece_value(PieceType::queen));
                    const int bad_capture_penalty =
                        see < 0 && !metadata.gives_check ? kBadCapturePenalty : 0;
                    metadata.ordering_score =
                        candidate.priority + see * kSeeOrderingWeight - bad_capture_penalty;
                } else {
                    metadata.ordering_score = ordering_.priority(
                        state_, metadata, std::nullopt, history_context_);
                }
                return metadata;
            }
        }
        advance_stage();
    }
}

} // namespace koi::detail
