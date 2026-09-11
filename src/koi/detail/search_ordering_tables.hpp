#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "koi/game_state.hpp"

namespace koi::detail {

// SearchOrderingTables owns the adaptive state for one worker's move ordering.
// It intentionally has no knowledge of search-policy decisions or recursion.
class SearchOrderingTables {
public:
    void clear() noexcept;

    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int killer_rank(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, Move move, std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        Color side, Move previous_move, Move move) const noexcept;

    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, Move move, int ply, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;

private:
    static constexpr int kMaximumPly = 64;
    static constexpr std::size_t kContinuationHistorySize = 16 * 1024;

    std::array<std::array<Move, 2>, kMaximumPly> killers_{};
    std::array<std::array<int, 64 * 64>, 2> history_{};
    std::array<std::array<Move, 64 * 64>, 2> counter_moves_{};
    std::array<std::array<int, 64 * 64>, 2> counter_confidence_{};
    std::array<int, kContinuationHistorySize> continuation_history_{};
};

} // namespace koi::detail
