#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "koi/game_state.hpp"

namespace koi::detail {

[[nodiscard]] std::uint32_t move_tie_break_key(Move move) noexcept;

// SearchMoveOrdering is deliberately private to the search implementation.
// It does not change the rules or UCI interfaces.
class SearchMoveOrdering {
public:
    void clear() noexcept;
    void order(const GameState& state, std::vector<Move>& moves,
               std::optional<Move> tt_move, int ply) const;
    void order(const GameState& state, MoveMetadataList& moves,
               std::optional<Move> tt_move, int ply) const;
    void order(const GameState& state, std::vector<MoveMetadata>& moves,
               std::optional<Move> tt_move, int ply) const;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth) noexcept;
    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;

private:
    static constexpr int kMaximumPly = 64;

    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move, int ply) const;

    struct ScoredMove {
        MoveMetadata metadata;
        int priority;
        std::uint32_t tie_break;
    };

    std::array<std::array<Move, 2>, kMaximumPly> killers_{};
    std::array<std::array<int, 64 * 64>, 2> history_{};
    mutable std::vector<ScoredMove> scored_moves_;
};

} // namespace koi::detail
