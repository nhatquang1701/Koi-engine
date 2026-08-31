#include "koi/detail/search_ordering.hpp"

#include <algorithm>
#include <limits>

namespace koi::detail {
namespace {

constexpr int kTtMovePriority = 1'000'000;
constexpr int kCapturePriority = 500'000;
constexpr int kPromotionPriority = 400'000;
constexpr int kKillerPriority = 300'000;
constexpr int kMaximumHistoryScore = kKillerPriority - 1;
constexpr int kMaximumPly = 64;

int color_index(Color color) noexcept {
    return color == Color::white ? 0 : 1;
}

int piece_value(PieceType type) noexcept {
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

int promotion_value(Promotion promotion) noexcept {
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

int normalized_ply(int ply) noexcept {
    return std::clamp(ply, 0, kMaximumPly - 1);
}

std::size_t move_index(Move move) noexcept {
    return static_cast<std::size_t>(move.from().index()) * 64U + move.to().index();
}

} // namespace

std::uint32_t move_tie_break_key(Move move) noexcept {
    const auto square_key = [](Square square) noexcept -> std::uint32_t {
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
    killers_ = {};
    history_ = {};
    scored_moves_.clear();
}

void SearchMoveOrdering::order(const GameState& state, std::vector<Move>& moves,
                               std::optional<Move> tt_move, int ply) const {
    std::vector<MoveMetadata> metadata;
    metadata.reserve(moves.size());
    for (const Move move : moves) {
        if (const auto described = state.describe_move(move); described.has_value()) {
            metadata.push_back(*described);
        }
    }
    order(state, metadata, tt_move, ply);
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        moves[index] = metadata[index].move;
    }
}

void SearchMoveOrdering::order(const GameState& state, MoveMetadataList& moves,
                               std::optional<Move> tt_move, int ply) const {
    scored_moves_.clear();
    scored_moves_.reserve(moves.size());
    for (const MoveMetadata& metadata : moves) {
        scored_moves_.push_back(ScoredMove{metadata, priority(state, metadata, tt_move, ply),
                                           move_tie_break_key(metadata.move)});
    }

    std::sort(scored_moves_.begin(), scored_moves_.end(), [](const ScoredMove& lhs, const ScoredMove& rhs) {
        return lhs.priority != rhs.priority ? lhs.priority > rhs.priority : lhs.tie_break < rhs.tie_break;
    });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves_[index].metadata;
    }
}

int SearchMoveOrdering::priority(const GameState& state, const MoveMetadata& metadata,
                                 std::optional<Move> tt_move, int ply) const {
    const Move move = metadata.move;
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }

    if (metadata.is_capture()) {
        const int victim_value = piece_value(metadata.captured_piece);
        const int attacker_value = piece_value(metadata.moving_piece);
        return kCapturePriority + (victim_value * 16) - attacker_value + promotion_value(move.promotion());
    }
    if (move.promotion() != Promotion::none) {
        return kPromotionPriority + promotion_value(move.promotion());
    }

    const int checked_ply = normalized_ply(ply);
    if (killers_[static_cast<std::size_t>(checked_ply)][0] == move) {
        return kKillerPriority + 1;
    }
    if (killers_[static_cast<std::size_t>(checked_ply)][1] == move) {
        return kKillerPriority;
    }
    return history_[static_cast<std::size_t>(color_index(state.side_to_move()))][move_index(move)];
}

void SearchMoveOrdering::order(const GameState& state, std::vector<MoveMetadata>& moves,
                               std::optional<Move> tt_move,
                               int ply) const {
    scored_moves_.clear();
    scored_moves_.reserve(moves.size());
    for (const MoveMetadata& metadata : moves) {
        scored_moves_.push_back(ScoredMove{metadata, priority(state, metadata, tt_move, ply),
                                           move_tie_break_key(metadata.move)});
    }

    std::sort(scored_moves_.begin(), scored_moves_.end(), [](const ScoredMove& lhs, const ScoredMove& rhs) {
        return lhs.priority != rhs.priority ? lhs.priority > rhs.priority : lhs.tie_break < rhs.tie_break;
    });

    for (std::size_t index = 0; index < moves.size(); ++index) {
        moves[index] = scored_moves_[index].metadata;
    }
}

void SearchMoveOrdering::record_quiet_cutoff(Color side, Move move, int ply, int depth) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }

    const int checked_ply = normalized_ply(ply);
    auto& killers = killers_[static_cast<std::size_t>(checked_ply)];
    if (killers[0] != move) {
        killers[1] = killers[0];
        killers[0] = move;
    }

    int& history = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    history = std::min(kMaximumHistoryScore - bonus, history) + bonus;
}

} // namespace koi::detail
