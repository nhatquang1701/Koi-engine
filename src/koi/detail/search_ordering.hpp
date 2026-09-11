#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/detail/search_ordering_tables.hpp"

namespace koi::detail {

[[nodiscard]] std::uint32_t move_tie_break_key(Move move) noexcept;

// SearchMoveOrdering is deliberately private to the search implementation.
// It does not change the rules or UCI interfaces.
class SearchMoveOrdering {
public:
    void clear() noexcept;
    void order(const GameState& state, std::vector<Move>& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void order(const GameState& state, MoveMetadataList& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void order(const GameState& state, std::vector<MoveMetadata>& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, Move move, int ply, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(Color side, Move move,
                                          std::optional<Move> previous_move = std::nullopt) const noexcept;

private:
    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move, int ply,
                               std::optional<Move> previous_move) const;

    struct ScoredMove {
        MoveMetadata metadata{};
        int priority = 0;
        std::uint32_t tie_break = 0;
    };

    SearchOrderingTables tables_;
    mutable std::array<ScoredMove, kMaximumLegalMoves> scored_moves_{};
    mutable std::size_t scored_move_count_ = 0;
};

} // namespace koi::detail
