#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/detail/search_ordering_tables.hpp"

namespace koi::detail {

[[nodiscard]] std::uint32_t move_tie_break_key(Move move) noexcept;

class SearchMovePicker;

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
    void order(const GameState& state, MoveMetadataList& moves,
               std::optional<Move> tt_move,
               const SearchHistoryContext& history_context) const;
    void order(const GameState& state, std::vector<MoveMetadata>& moves,
               std::optional<Move> tt_move, int ply,
               std::optional<Move> previous_move = std::nullopt) const;
    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, Move move, int ply, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_cutoff(Color side, const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_quiet_best(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& history_context) noexcept;
    void record_quiet_fail(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& history_context) noexcept;
    void record_capture_cutoff(const MoveMetadata& metadata, int depth,
                               const SearchHistoryContext& history_context) noexcept;
    void record_capture_best(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_capture_fail(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& history_context) noexcept;
    void record_parent_fail_low(Color side, Move move, PieceType piece, int parent_ply,
                                const SearchHistoryContext& history_context, int depth) noexcept;
    void record_parent_refuted(Color side, Move move, PieceType piece, int parent_ply,
                               const SearchHistoryContext& history_context, int depth) noexcept;
    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(Color side, Move move,
                                          std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] int quiet_history_score(Color side, const MoveMetadata& metadata,
                                           const SearchHistoryContext& history_context) const noexcept;
    [[nodiscard]] int continuation_history_score(
        const MoveMetadata& metadata, const SearchHistoryContext& history_context) const noexcept;
    [[nodiscard]] int capture_history_score(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        Color side, Move previous_move, Move move) const noexcept;
    [[nodiscard]] int correction_value(std::uint64_t pawn_key, std::uint64_t material_key,
                                       std::uint64_t king_key) const noexcept;
    void update_correction(std::uint64_t pawn_key, std::uint64_t material_key,
                           std::uint64_t king_key, int bonus) noexcept;

private:
    friend class SearchMovePicker;

    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move, int ply,
                               std::optional<Move> previous_move) const;
    [[nodiscard]] int priority(const GameState& state, const MoveMetadata& metadata,
                               std::optional<Move> tt_move,
                               const SearchHistoryContext& history_context) const;

    struct ScoredMove {
        MoveMetadata metadata{};
        int priority = 0;
        std::uint32_t tie_break = 0;
    };

    SearchOrderingTables tables_;
    mutable std::array<ScoredMove, kMaximumLegalMoves> scored_moves_{};
    mutable std::size_t scored_move_count_ = 0;
};

// SearchMovePicker mirrors Stockfish's staged MovePicker while retaining
// Koi's fixed metadata container and deterministic tie-breaking. It owns only
// bounded stack storage; all history state remains in the parent ordering
// object and all move legality remains authoritative in GameState metadata.
class SearchMovePicker {
public:
    enum class Mode : std::uint8_t { main, quiescence, evasion };

    SearchMovePicker(const SearchMoveOrdering& ordering, const GameState& state,
                     const MoveMetadataList& moves, std::optional<Move> tt_move,
                     const SearchHistoryContext& history_context, int ply, Mode mode,
                     Move excluded_move = Move::no_move()) noexcept;

    [[nodiscard]] std::optional<MoveMetadata> next();

    // Called by a cut node after enough forcing moves have been searched. The
    // current candidate is preserved, while later quiet stages are skipped in
    // the same direction as Stockfish's skip_quiet_moves().
    void skip_quiet_moves() noexcept { skip_quiets_ = true; }

private:
    enum class Stage : std::uint8_t {
        tt,
        good_captures,
        good_quiets,
        bad_captures,
        bad_quiets,
        quiet_checks,
        evasions,
        done,
    };

    struct Candidate {
        std::uint16_t source_index = 0;
        std::uint8_t stage_rank = 0;
        int priority = 0;
        std::uint32_t tie_break = 0;
    };

    [[nodiscard]] bool is_excluded(std::size_t index) const noexcept;
    [[nodiscard]] bool is_good_capture(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] Stage first_stage() const noexcept;
    [[nodiscard]] MoveMetadata materialize(std::size_t index);
    void prepare_candidates();
    void advance_stage() noexcept;
    [[nodiscard]] std::optional<MoveMetadata> emit_tt();

    const SearchMoveOrdering& ordering_;
    const GameState& state_;
    const MoveMetadataList& moves_;
    SearchHistoryContext history_context_;
    std::optional<Move> tt_move_;
    Move excluded_move_;
    int ply_ = 0;
    Mode mode_ = Mode::main;
    Stage stage_ = Stage::done;
    std::bitset<kMaximumLegalMoves> emitted_{};
    std::bitset<kMaximumLegalMoves> see_computed_{};
    std::bitset<kMaximumLegalMoves> capture_check_computed_{};
    std::bitset<kMaximumLegalMoves> capture_checks_{};
    std::array<std::int16_t, kMaximumLegalMoves> see_scores_{};
    std::array<Candidate, kMaximumLegalMoves> staged_{};
    // The good-capture pass is already ordered by the same history/MVV score
    // used by the bad-capture pass. Keep only source indices for deferred
    // captures instead of copying the complete Candidate record a second time.
    std::array<std::uint16_t, kMaximumLegalMoves> deferred_bad_capture_indices_{};
    std::size_t candidate_count_ = 0;
    std::size_t candidate_index_ = 0;
    std::size_t deferred_bad_capture_count_ = 0;
    std::size_t deferred_bad_capture_index_ = 0;
    bool candidates_prepared_ = false;
    bool tt_emitted_ = false;
    bool skip_quiets_ = false;
};

} // namespace koi::detail
