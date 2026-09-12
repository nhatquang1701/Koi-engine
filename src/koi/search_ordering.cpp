#include "koi/detail/search_ordering.hpp"

#include <algorithm>
#include <limits>

#include "koi/detail/static_exchange.hpp"

namespace koi::detail {
namespace {

constexpr int kTtMovePriority = 1'000'000;
constexpr int kCapturePriority = 500'000;
constexpr int kPromotionPriority = 400'000;
constexpr int kCheckingMovePriority = 350'000;
constexpr int kKillerPriority = 300'000;
constexpr int kCounterMovePriority = kKillerPriority - 1;
constexpr int kSeeOrderingWeight = 12;
// Stockfish's staged picker searches good captures, then quiets, and only
// then poisoned captures. Keep bad captures available for tactical recovery,
// but place them below checks and the strongest quiet history.
constexpr int kBadCapturePenalty = 210'000;
constexpr int kCaptureHistoryWeight = 2;

int piece_value(const PieceType type) noexcept {
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
    case PieceType::none:
        return 0;
    }
    return 0;
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
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(state, metadata),
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
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(state, metadata),
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
        const int see = std::clamp(static_cast<int>(metadata.see_score),
                                   -piece_value(PieceType::queen), piece_value(PieceType::queen));
        const int capture_history = tables_.capture_history_score(metadata);
        const int bad_capture_penalty = see < 0 && !metadata.gives_check ?
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
        const int see = std::clamp(static_cast<int>(metadata.see_score),
                                   -piece_value(PieceType::queen), piece_value(PieceType::queen));
        const int capture_history = tables_.capture_history_score(metadata);
        const int bad_capture_penalty = see < 0 && !metadata.gives_check ?
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
    for (MoveMetadata& metadata : moves) {
        if (!metadata.see_computed && metadata.is_capture()) {
            metadata.see_score = static_cast<std::int16_t>(std::clamp(
                static_exchange_gain(state, metadata),
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

int SearchMoveOrdering::capture_history_score(
    const MoveMetadata& metadata) const noexcept {
    return tables_.capture_history_score(metadata);
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

} // namespace koi::detail
