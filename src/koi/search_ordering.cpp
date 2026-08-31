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

void SearchMoveOrdering::clear() noexcept {
    killers_ = {};
    history_ = {};
}

int SearchMoveOrdering::priority(const GameState& state, Move move, std::optional<Move> tt_move, int ply) const {
    if (tt_move.has_value() && move == *tt_move) {
        return kTtMovePriority;
    }

    if (state.is_capture(move)) {
        const Piece victim = state.piece_at(move.to());
        const int victim_value = victim.empty() ? piece_value(PieceType::pawn) : piece_value(victim.type);
        const int attacker_value = piece_value(state.piece_at(move.from()).type);
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

void SearchMoveOrdering::order(const GameState& state, std::vector<Move>& moves, std::optional<Move> tt_move,
                               int ply) const {
    std::stable_sort(moves.begin(), moves.end(), [&state, tt_move, ply, this](const Move& lhs, const Move& rhs) {
        const int lhs_priority = priority(state, lhs, tt_move, ply);
        const int rhs_priority = priority(state, rhs, tt_move, ply);
        return lhs_priority != rhs_priority ? lhs_priority > rhs_priority : lhs.uci() < rhs.uci();
    });
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
