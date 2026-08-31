#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "koi/move.hpp"

namespace koi {

enum class PositionErrorCode : std::uint8_t { malformed_fen, illegal_position };

struct PositionError {
    PositionErrorCode code;
    std::string message;
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
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    bool make_move(const Move& move) noexcept;
    bool unmake_move() noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint64_t position_key() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
