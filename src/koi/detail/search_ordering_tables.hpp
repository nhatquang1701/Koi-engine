#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "koi/game_state.hpp"

namespace koi::detail {

inline constexpr std::size_t kSearchContinuationPlies = 6;

// The recursive search supplies the short move suffix that is available at a
// node.  Keeping this as a value object makes all ordering tables worker-local
// and avoids exposing SearchStack internals to the ordering implementation.
struct SearchHistoryContext {
    std::array<Move, kSearchContinuationPlies> continuation_moves{};
    std::size_t count = 0;
    int ply = 0;
    // GameState does not expose a separate pawn key.  The search uses the
    // position key as a stable, collision-tolerant pawn-structure bucket;
    // the move dimensions remain explicit in the table index.
    std::uint64_t pawn_key = 0;
};

// SearchOrderingTables owns the adaptive state for one worker's move ordering.
// It intentionally has no knowledge of search-policy decisions or recursion.
class SearchOrderingTables {
public:
    SearchOrderingTables();
    ~SearchOrderingTables();
    SearchOrderingTables(const SearchOrderingTables&) = delete;
    SearchOrderingTables& operator=(const SearchOrderingTables&) = delete;

    void clear() noexcept;

    [[nodiscard]] bool is_killer(Move move, int ply) const noexcept;
    [[nodiscard]] int killer_rank(Move move, int ply) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, Move move, std::optional<Move> previous_move = std::nullopt) const noexcept;
    [[nodiscard]] int quiet_history_score(
        Color side, const MoveMetadata& metadata,
        const SearchHistoryContext& context) const noexcept;
    [[nodiscard]] int capture_history_score(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] bool is_proven_counter_move(
        Color side, Move previous_move, Move move) const noexcept;

    void record_quiet_cutoff(Color side, Move move, int ply, int depth,
                             std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_cutoff(Color side, const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;
    void record_quiet_best(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& context) noexcept;
    void record_quiet_fail(Color side, Move move, int ply, int depth,
                           std::optional<Move> previous_move = std::nullopt) noexcept;
    void record_quiet_fail(Color side, const MoveMetadata& metadata, int depth,
                           const SearchHistoryContext& context) noexcept;
    void record_capture_cutoff(const MoveMetadata& metadata, int depth,
                               const SearchHistoryContext& context) noexcept;
    void record_capture_best(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;
    void record_capture_fail(const MoveMetadata& metadata, int depth,
                             const SearchHistoryContext& context) noexcept;

private:
    static constexpr int kMaximumPly = 64;
    static constexpr std::size_t kMoveTableSize = 64 * 64;
    static constexpr std::size_t kPieceTypeCount = 7;
    static constexpr std::size_t kCapturedPieceCount = 7;
    static constexpr std::size_t kCaptureHistorySize =
        kPieceTypeCount * 64 * kCapturedPieceCount;
    static constexpr std::size_t kPieceToHistorySize = kPieceTypeCount * 64;
    static constexpr std::size_t kLowPlyHistoryPlies = 5;
    static constexpr std::size_t kPawnHistorySize = 32 * 1024;
    static constexpr std::size_t kContinuationHistorySize = 16 * 1024;

    [[nodiscard]] static int piece_index(PieceType type) noexcept;
    [[nodiscard]] static std::size_t move_index(Move move) noexcept;
    [[nodiscard]] static std::size_t capture_index(const MoveMetadata& metadata) noexcept;
    [[nodiscard]] static std::size_t piece_to_index(PieceType piece, Square to) noexcept;
    [[nodiscard]] static std::size_t continuation_index(Move previous_move,
                                                         Move move) noexcept;
    [[nodiscard]] static std::size_t pawn_index(const SearchHistoryContext& context,
                                                 PieceType piece, Square to) noexcept;
    [[nodiscard]] static int normalized_ply(int ply) noexcept;
    static void update_history(int& score, int delta, int maximum) noexcept;

    std::array<std::array<Move, 2>, kMaximumPly> killers_{};
    std::array<std::array<int, kMoveTableSize>, 2> history_{};
    std::array<std::array<Move, kMoveTableSize>, 2> counter_moves_{};
    std::array<std::array<int, kMoveTableSize>, 2> counter_confidence_{};
    std::array<int, kCaptureHistorySize> capture_history_{};
    std::array<int, kPieceToHistorySize> piece_to_history_{};
    std::array<int, kLowPlyHistoryPlies * kMoveTableSize> low_ply_history_{};
    std::array<int, kContinuationHistorySize> continuation_history_{};
    std::unique_ptr<int[]> multi_ply_continuation_history_;
    std::unique_ptr<int[]> pawn_history_;
};

} // namespace koi::detail
