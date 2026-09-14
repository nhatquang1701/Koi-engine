#include "koi/detail/search_ordering_tables.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace koi::detail {
namespace {

constexpr int kCounterMovePriority = 300'000 - 1;
constexpr int kMaximumHistoryScore = kCounterMovePriority - 1;
constexpr int kHistoryTableMaximum = 16'384;
constexpr int kCaptureHistoryMaximum = 16'384;
constexpr int kCounterMoveMinimumConfidence = 7 * 7;
constexpr int kMaximumPly = 64;

// These priors match the neutral starting point used by Stockfish 19's
// search histories.  The fixed tables are value-initialized, but the two
// worker-local dynamic tables need explicit initialization before their first
// node is searched.
constexpr int kInitialCaptureHistory = -742;
constexpr int kInitialPieceToHistory = -586;
constexpr int kInitialContinuationHistory = -586;
constexpr int kInitialPawnHistory = -1338;

int color_index(const Color color) noexcept {
    return color == Color::white ? 0 : 1;
}

std::size_t table_move_index(const Move move) noexcept {
    if (move.is_no_move() || move.from().index() >= Square::kInvalid ||
        move.to().index() >= Square::kInvalid) {
        return 0;
    }
    return static_cast<std::size_t>(move.from().index()) * 64U + move.to().index();
}

int clamp_history(const std::int64_t value, const int maximum) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(value, -maximum, maximum));
}

std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

} // namespace

SearchOrderingTables::SearchOrderingTables()
    : multi_ply_continuation_history_(std::make_unique<int[]>(
          kSearchContinuationPlies * kContinuationHistorySize)),
      pawn_history_(std::make_unique<int[]>(kPawnHistorySize)) {
    // `clear()` intentionally retains its historical zeroed-reset contract
    // for diagnostics.  A newly created worker, however, should begin with
    // the calibrated Stockfish-style priors rather than indeterminate heap
    // contents.
    capture_history_.fill(kInitialCaptureHistory);
    piece_to_history_.fill(kInitialPieceToHistory);
    low_ply_history_.fill(102);
    continuation_history_.fill(kInitialContinuationHistory);
    std::fill_n(multi_ply_continuation_history_.get(),
                kSearchContinuationPlies * kContinuationHistorySize,
                kInitialContinuationHistory);
    std::fill_n(pawn_history_.get(), kPawnHistorySize, kInitialPawnHistory);
}

SearchOrderingTables::~SearchOrderingTables() = default;

int SearchOrderingTables::piece_index(const PieceType type) noexcept {
    const int index = static_cast<int>(type);
    return index >= 0 && index < static_cast<int>(kPieceTypeCount) ? index : 0;
}

std::size_t SearchOrderingTables::move_index(const Move move) noexcept {
    return table_move_index(move);
}

std::size_t SearchOrderingTables::capture_index(const MoveMetadata& metadata) noexcept {
    const std::size_t piece = static_cast<std::size_t>(piece_index(metadata.moving_piece));
    const std::size_t to = metadata.move.to().index() < Square::kInvalid ?
        static_cast<std::size_t>(metadata.move.to().index()) : 0U;
    const std::size_t captured = static_cast<std::size_t>(piece_index(metadata.captured_piece));
    return (piece * 64U + to) * kCapturedPieceCount + captured;
}

std::size_t SearchOrderingTables::piece_to_index(const PieceType piece,
                                                 const Square to) noexcept {
    const std::size_t square = to.index() < Square::kInvalid ?
        static_cast<std::size_t>(to.index()) : 0U;
    return static_cast<std::size_t>(piece_index(piece)) * 64U + square;
}

std::size_t SearchOrderingTables::continuation_index(const Move previous_move,
                                                     const Move move) noexcept {
    // Keep the original hash shape.  A few diagnostics intentionally exercise
    // its collisions, while the exact counter table below remains collision-free.
    constexpr std::size_t mask = kContinuationHistorySize - 1;
    return (move_index(previous_move) * 131U + move_index(move) * 17U) & mask;
}

std::size_t SearchOrderingTables::pawn_index(const SearchHistoryContext& context,
                                             const PieceType piece,
                                             const Square to) noexcept {
    const std::uint64_t dimensions =
        (static_cast<std::uint64_t>(piece_index(piece)) << 8U) |
        static_cast<std::uint64_t>(to.index() < Square::kInvalid ? to.index() : 0U);
    return static_cast<std::size_t>(mix64(context.pawn_key ^ dimensions) &
                                    (kPawnHistorySize - 1));
}

int SearchOrderingTables::normalized_ply(const int ply) noexcept {
    return std::clamp(ply, 0, kMaximumPly - 1);
}

void SearchOrderingTables::update_history(int& score, const int delta,
                                           const int maximum) noexcept {
    const std::int64_t wide_score = score;
    const std::int64_t wide_delta = delta;
    const std::int64_t wide_abs_delta = wide_delta < 0 ? -wide_delta : wide_delta;
    const std::int64_t updated = wide_score + wide_delta -
        (wide_score * wide_abs_delta) / std::max(1, maximum);
    score = clamp_history(updated, maximum);
}

void SearchOrderingTables::clear() noexcept {
    killers_ = {};
    history_ = {};
    counter_moves_ = {};
    counter_confidence_ = {};
    capture_history_ = {};
    piece_to_history_ = {};
    low_ply_history_ = {};
    continuation_history_ = {};
    if (multi_ply_continuation_history_ != nullptr) {
        std::fill_n(multi_ply_continuation_history_.get(),
                    kSearchContinuationPlies * kContinuationHistorySize, 0);
    }
    if (pawn_history_ != nullptr) {
        std::fill_n(pawn_history_.get(), kPawnHistorySize, 0);
    }
}

int SearchOrderingTables::killer_rank(const Move move, const int ply) const noexcept {
    const auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
    if (killers[0] == move) {
        return 2;
    }
    if (killers[1] == move) {
        return 1;
    }
    return 0;
}

bool SearchOrderingTables::is_killer(const Move move, const int ply) const noexcept {
    return killer_rank(move, ply) != 0;
}

int SearchOrderingTables::quiet_history_score(
    const Color side, const Move move, const std::optional<Move> previous_move) const noexcept {
    if (move.is_no_move()) {
        return 0;
    }
    int score = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        score += continuation_history_[continuation_index(*previous_move, move)];
    }
    return std::clamp(score, -kMaximumHistoryScore, kMaximumHistoryScore);
}

int SearchOrderingTables::quiet_history_score(
    const Color side, const MoveMetadata& metadata,
    const SearchHistoryContext& context) const noexcept {
    if (metadata.move.is_no_move()) {
        return 0;
    }

    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    std::int64_t score = history_[side_index][move_index(metadata.move)];
    score += piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())];
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        score += low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                  move_index(metadata.move)];
    }

    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    // The distance-zero pair is the compact continuation table above.  The
    // multi-ply table starts at the second predecessor so a move is not
    // counted twice merely because both storage forms contain the same key.
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        const std::size_t slot = distance * kContinuationHistorySize +
            continuation_index(previous, metadata.move);
        score += multi_ply_continuation_history_[slot] /
            static_cast<int>(distance + 1);
    }

    if (context.count > 0 && !context.continuation_moves[0].is_no_move()) {
        score += continuation_history_[continuation_index(
            context.continuation_moves[0], metadata.move)];
    }
    score += pawn_history_[pawn_index(context, metadata.moving_piece, metadata.move.to())] / 2;

    return clamp_history(score, kMaximumHistoryScore);
}

int SearchOrderingTables::continuation_history_score(
    const MoveMetadata& metadata, const SearchHistoryContext& context) const noexcept {
    if (metadata.move.is_no_move()) {
        return 0;
    }

    std::int64_t score = 0;
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        score += multi_ply_continuation_history_[
            distance * kContinuationHistorySize + continuation_index(previous, metadata.move)] /
            static_cast<int>(distance + 1);
    }
    if (move_count > 0 && !context.continuation_moves[0].is_no_move()) {
        score += continuation_history_[continuation_index(
            context.continuation_moves[0], metadata.move)];
    }
    return clamp_history(score, kMaximumHistoryScore);
}

int SearchOrderingTables::capture_history_score(const MoveMetadata& metadata) const noexcept {
    if (!metadata.is_capture()) {
        return 0;
    }
    return capture_history_[capture_index(metadata)];
}

bool SearchOrderingTables::is_proven_counter_move(
    const Color side, const Move previous_move, const Move move) const noexcept {
    if (previous_move.is_no_move() || move.is_no_move()) {
        return false;
    }
    const std::size_t side_index = static_cast<std::size_t>(color_index(side));
    const std::size_t previous_index = move_index(previous_move);
    return counter_moves_[side_index][previous_index] == move &&
        counter_confidence_[side_index][previous_index] >= kCounterMoveMinimumConfidence;
}

void SearchOrderingTables::record_quiet_cutoff(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }

    auto& killers = killers_[static_cast<std::size_t>(normalized_ply(ply))];
    if (killers[0] != move) {
        killers[1] = killers[0];
        killers[0] = move;
    }

    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    int& history = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    update_history(history, bonus, kMaximumHistoryScore);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        Move& counter_move = counter_moves_[side_index][previous_index];
        int& counter_confidence = counter_confidence_[side_index][previous_index];
        if (counter_move != move) {
            counter_move = move;
            counter_confidence = 0;
        }
        update_history(counter_confidence, bonus, kMaximumHistoryScore);
        update_history(continuation_history_[continuation_index(*previous_move, move)],
                       bonus * 2, kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_quiet_cutoff(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }

    record_quiet_cutoff(side, metadata.move, context.ply, depth,
                        context.count > 0 ? std::optional<Move>{context.continuation_moves[0]} :
                                            std::nullopt);
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)], bonus,
                       kHistoryTableMaximum);
    }

    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(multi_ply_continuation_history_[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       bonus, kHistoryTableMaximum);
    }
    update_history(pawn_history_[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_quiet_best(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    // A best move that did not cut off still deserves a small positive update;
    // this is the quiet analogue of Stockfish's quiet-best history.
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }
    const int bonus = std::max(1, std::clamp(depth, 1, kMaximumPly));
    int& history = history_[static_cast<std::size_t>(color_index(side))][move_index(metadata.move)];
    update_history(history, bonus, kMaximumHistoryScore);
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)],
                       std::max(1, bonus * 3 / 4), kHistoryTableMaximum);
    }
    if (context.count > 0 && !context.continuation_moves[0].is_no_move()) {
        update_history(continuation_history_[continuation_index(
                           context.continuation_moves[0], metadata.move)],
                       std::max(1, bonus * 3 / 4), kHistoryTableMaximum);
    }
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(multi_ply_continuation_history_[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       std::max(1, bonus / static_cast<int>(distance + 1)),
                       kHistoryTableMaximum);
    }
    update_history(pawn_history_[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   bonus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_quiet_fail(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int malus = -(depth_bonus * depth_bonus);
    update_history(history_[static_cast<std::size_t>(color_index(side))][move_index(move)],
                   malus, kMaximumHistoryScore);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        if (counter_moves_[side_index][previous_index] == move) {
            counter_confidence_[side_index][previous_index] = 0;
        }
        update_history(continuation_history_[continuation_index(*previous_move, move)],
                       malus, kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_quiet_fail(
    const Color side, const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (metadata.move.is_no_move() || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none) {
        return;
    }
    record_quiet_fail(side, metadata.move, context.ply, depth,
                      context.count > 0 ? std::optional<Move>{context.continuation_moves[0]} :
                                          std::nullopt);
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int malus = -(depth_bonus * depth_bonus);
    update_history(piece_to_history_[piece_to_index(metadata.moving_piece, metadata.move.to())],
                   malus, kHistoryTableMaximum);
    if (context.ply < static_cast<int>(kLowPlyHistoryPlies)) {
        update_history(low_ply_history_[static_cast<std::size_t>(context.ply) * kMoveTableSize +
                                        move_index(metadata.move)], malus,
                       kHistoryTableMaximum);
    }

    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(multi_ply_continuation_history_[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       malus, kHistoryTableMaximum);
    }
    update_history(pawn_history_[pawn_index(context, metadata.moving_piece, metadata.move.to())],
                   malus, kHistoryTableMaximum);
}

void SearchOrderingTables::record_capture_cutoff(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& context) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    update_history(capture_history_[capture_index(metadata)], depth_bonus * depth_bonus,
                   kCaptureHistoryMaximum);
    // Captures also provide useful continuation information, but keep this
    // update weaker than the quiet history updates so tactical noise cannot
    // permanently dominate a quiet move ordering table.
    if (context.count > 0 && !context.continuation_moves[0].is_no_move()) {
        update_history(continuation_history_[continuation_index(
                           context.continuation_moves[0], metadata.move)],
                       depth_bonus, kHistoryTableMaximum);
    }
    const std::size_t move_count = std::min(context.count, kSearchContinuationPlies);
    for (std::size_t distance = 1; distance < move_count; ++distance) {
        const Move previous = context.continuation_moves[distance];
        if (previous.is_no_move()) {
            continue;
        }
        update_history(multi_ply_continuation_history_[
                           distance * kContinuationHistorySize +
                           continuation_index(previous, metadata.move)],
                       std::max(1, depth_bonus / static_cast<int>(distance + 1)),
                       kHistoryTableMaximum);
    }
}

void SearchOrderingTables::record_capture_best(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& /*context*/) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    update_history(capture_history_[capture_index(metadata)],
                   std::max(1, std::clamp(depth, 1, kMaximumPly)),
                   kCaptureHistoryMaximum);
}

void SearchOrderingTables::record_capture_fail(
    const MoveMetadata& metadata, const int depth,
    const SearchHistoryContext& /*context*/) noexcept {
    if (!metadata.is_capture()) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    update_history(capture_history_[capture_index(metadata)], -(depth_bonus * depth_bonus),
                   kCaptureHistoryMaximum);
}

} // namespace koi::detail
