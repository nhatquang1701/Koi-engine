#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "koi/move.hpp"

namespace koi {

class GameState;

enum class PositionErrorCode : std::uint8_t { malformed_fen, illegal_position };

struct PositionError {
    PositionErrorCode code;
    std::string message;
};

enum class MoveKind : std::uint8_t { quiet, capture, en_passant, castling, promotion };

struct MoveMetadata {
    Move move;
    PieceType moving_piece = PieceType::none;
    PieceType captured_piece = PieceType::none;
    MoveKind kind = MoveKind::quiet;
    bool gives_check = false;

    [[nodiscard]] constexpr bool is_capture() const noexcept {
        return kind == MoveKind::capture || kind == MoveKind::en_passant;
    }
};

namespace detail {
[[nodiscard]] int static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;
}

inline constexpr std::size_t kMaximumLegalMoves = 256;

class MoveMetadataList {
public:
    using iterator = std::array<MoveMetadata, kMaximumLegalMoves>::iterator;
    using const_iterator = std::array<MoveMetadata, kMaximumLegalMoves>::const_iterator;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr MoveMetadata& front() noexcept { return storage_[0]; }
    [[nodiscard]] constexpr const MoveMetadata& front() const noexcept { return storage_[0]; }
    [[nodiscard]] constexpr MoveMetadata& back() noexcept { return storage_[size_ - 1]; }
    [[nodiscard]] constexpr const MoveMetadata& back() const noexcept { return storage_[size_ - 1]; }
    [[nodiscard]] constexpr MoveMetadata& operator[](std::size_t index) noexcept { return storage_[index]; }
    [[nodiscard]] constexpr const MoveMetadata& operator[](std::size_t index) const noexcept {
        return storage_[index];
    }
    [[nodiscard]] constexpr iterator begin() noexcept { return storage_.begin(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return storage_.begin(); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return storage_.cbegin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return storage_.begin() + size_; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return storage_.begin() + size_; }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return storage_.cbegin() + size_; }

    constexpr void clear() noexcept { size_ = 0; }

    [[nodiscard]] constexpr bool push_back(const MoveMetadata& metadata) noexcept {
        if (size_ >= kMaximumLegalMoves) {
            return false;
        }
        storage_[size_++] = metadata;
        return true;
    }

    constexpr void resize(std::size_t size) noexcept {
        size_ = std::min(size, kMaximumLegalMoves);
    }

private:
    std::array<MoveMetadata, kMaximumLegalMoves> storage_{};
    std::size_t size_ = 0;
};

struct PositionFeatures {
    std::array<Piece, 64> board{};
    std::array<std::uint64_t, 2> attacked_squares{};
    std::array<std::uint16_t, 2> mobility{};
    std::array<Square, 2> king_squares{};
    std::array<std::uint8_t, 2> pawn_file_masks{};
    std::uint8_t game_phase = 0;
    Color side_to_move = Color::white;
};

class GameState {
public:
    GameState();
    GameState(const GameState&);
    GameState(GameState&&) noexcept;
    GameState& operator=(const GameState&);
    GameState& operator=(GameState&&) noexcept;
    ~GameState();

    [[nodiscard]] static GameState startpos();
    [[nodiscard]] static std::expected<GameState, PositionError> from_fen(std::string_view fen);
    [[nodiscard]] std::string fen() const;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] Piece piece_at(Square square) const noexcept;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    [[nodiscard]] std::vector<MoveMetadata> legal_moves_with_metadata() const;
    void legal_moves_with_metadata(MoveMetadataList& moves,
                                   bool include_check_flags = true) const noexcept;
    // Generates all legal evasions while checked, or only captures,
    // promotions, and checking moves otherwise. The return value reports
    // whether any legal move exists, even when a quiet non-checking move was
    // intentionally omitted from the output.
    [[nodiscard]] bool legal_tactical_moves_with_metadata(
        MoveMetadataList& moves, bool include_quiet_checks = true) const noexcept;
    [[nodiscard]] std::optional<MoveMetadata> describe_move(const Move&) const noexcept;
    [[nodiscard]] PositionFeatures position_features() const noexcept;
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    bool make_move(const Move& move) noexcept;
    // Fast path for metadata returned by legal_moves_with_metadata() for this
    // unchanged position. The metadata must not be stale or fabricated.
    bool make_legal_move(const MoveMetadata& metadata) noexcept;
    bool unmake_move() noexcept;
    bool make_null_move() noexcept;
    bool unmake_null_move() noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept;
    // Draw conditions that can be checked after legal move generation. A
    // caller that needs checkmate/stalemate must still inspect legal_moves().
    [[nodiscard]] bool is_draw_by_rule() const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint64_t position_key() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;

private:
    [[nodiscard]] int direct_static_exchange_gain(const MoveMetadata&) const noexcept;

    friend int detail::static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
