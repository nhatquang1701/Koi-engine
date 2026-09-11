#pragma once

#include <chess.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/position.hpp"

namespace koi::detail {

class CompatibilityMirror final {
public:
    CompatibilityMirror() = default;
    CompatibilityMirror(const CompatibilityMirror&) = default;
    CompatibilityMirror& operator=(const CompatibilityMirror&) = default;

    [[nodiscard]] bool set_fen(std::string_view fen);
    [[nodiscard]] bool apply_move(const Move& move) noexcept;
    [[nodiscard]] bool apply_generated_move(const MoveMetadata& metadata,
                                            bool verify_legality) noexcept;
    [[nodiscard]] bool undo_move() noexcept;
    [[nodiscard]] bool apply_null_move() noexcept;
    [[nodiscard]] bool undo_null_move() noexcept;
    [[nodiscard]] bool last_move_is_null() const noexcept;
    [[nodiscard]] std::size_t history_size() const noexcept;

    [[nodiscard]] std::uint64_t position_key() const noexcept;
    [[nodiscard]] std::uint8_t castling_rights() const noexcept;
    [[nodiscard]] Square en_passant_square() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] std::size_t repetition_count() const noexcept;

    [[nodiscard]] std::vector<std::string> legal_move_strings() const;
    [[nodiscard]] std::string fen_for_comparison(const Position& native) const;
    [[nodiscard]] Square en_passant_square_for_comparison(const Position& native) const;
    [[nodiscard]] bool matches(const Position& native) const;
    [[nodiscard]] bool gives_check(const Move& move) const noexcept;

private:
    struct HistoryRecord {
        chess::Move move;
        bool null_move = false;
        std::uint64_t position_key = 0;
    };

    [[nodiscard]] chess::Move native_move_for(const Move& move) const noexcept;
    [[nodiscard]] chess::Move native_move_for(const MoveMetadata& metadata) const noexcept;
    [[nodiscard]] bool apply_native(chess::Move move, bool null_move) noexcept;

    chess::Board board_{};
    std::vector<HistoryRecord> history_;
};

} // namespace koi::detail
