#pragma once

#include <array>
#include <optional>
#include <vector>

#include "koi/game_state.hpp"

namespace koi::detail {

// SearchMoveOrdering is deliberately private to the search implementation.
// It does not change the rules or UCI interfaces.
class SearchMoveOrdering {
public:
    void clear() noexcept;
    void order(const GameState& state, std::vector<Move>& moves, std::optional<Move> tt_move, int ply) const;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth) noexcept;

private:
    static constexpr int kMaximumPly = 64;

    [[nodiscard]] int priority(const GameState& state, Move move, std::optional<Move> tt_move, int ply) const;

    std::array<std::array<Move, 2>, kMaximumPly> killers_{};
    std::array<std::array<int, 64 * 64>, 2> history_{};
};

} // namespace koi::detail
