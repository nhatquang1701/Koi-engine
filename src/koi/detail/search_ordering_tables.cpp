#include "koi/detail/search_ordering_tables.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace koi::detail {
namespace {

constexpr int kCounterMovePriority = 300'000 - 1;
constexpr int kMaximumHistoryScore = kCounterMovePriority - 1;
constexpr int kCounterMoveMinimumConfidence = 7 * 7;
constexpr int kMaximumPly = 64;

int color_index(const Color color) noexcept {
    return color == Color::white ? 0 : 1;
}

int normalized_ply(const int ply) noexcept {
    return std::clamp(ply, 0, kMaximumPly - 1);
}

std::size_t move_index(const Move move) noexcept {
    return static_cast<std::size_t>(move.from().index()) * 64U + move.to().index();
}

std::size_t continuation_index(const Move previous_move, const Move move) noexcept {
    constexpr std::size_t mask = 16 * 1024 - 1;
    return (move_index(previous_move) * 131U + move_index(move) * 17U) & mask;
}

void update_history(int& score, const int delta) noexcept {
    constexpr int maximum = kMaximumHistoryScore;
    const std::int64_t wide_score = score;
    const std::int64_t wide_delta = delta;
    const std::int64_t wide_abs_delta = wide_delta < 0 ? -wide_delta : wide_delta;
    const std::int64_t updated = wide_score + wide_delta -
        (wide_score * wide_abs_delta) / maximum;
    score = static_cast<int>(std::clamp<std::int64_t>(updated, -maximum, maximum));
}

} // namespace

void SearchOrderingTables::clear() noexcept {
    killers_ = {};
    history_ = {};
    counter_moves_ = {};
    counter_confidence_ = {};
    continuation_history_ = {};
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

    int& history = history_[static_cast<std::size_t>(color_index(side))][move_index(move)];
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int bonus = depth_bonus * depth_bonus;
    update_history(history, bonus);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        Move& counter_move = counter_moves_[side_index][previous_index];
        int& counter_confidence = counter_confidence_[side_index][previous_index];
        if (counter_move != move) {
            counter_move = move;
            counter_confidence = 0;
        }
        update_history(counter_confidence, bonus);
        update_history(continuation_history_[continuation_index(*previous_move, move)], bonus * 2);
    }
}

void SearchOrderingTables::record_quiet_fail(
    const Color side, const Move move, const int ply, const int depth,
    const std::optional<Move> previous_move) noexcept {
    if (move.is_no_move() || move.promotion() != Promotion::none) {
        return;
    }
    const int depth_bonus = std::clamp(depth, 1, kMaximumPly);
    const int malus = -(depth_bonus * depth_bonus);
    update_history(history_[static_cast<std::size_t>(color_index(side))][move_index(move)], malus);
    if (previous_move.has_value() && !previous_move->is_no_move()) {
        const std::size_t side_index = static_cast<std::size_t>(color_index(side));
        const std::size_t previous_index = move_index(*previous_move);
        if (counter_moves_[side_index][previous_index] == move) {
            counter_confidence_[side_index][previous_index] = 0;
        }
        update_history(continuation_history_[continuation_index(*previous_move, move)], malus);
    }
}

} // namespace koi::detail
