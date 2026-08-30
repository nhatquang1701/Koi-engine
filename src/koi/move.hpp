#pragma once

#include <string>

#include <chess.hpp>

namespace koi {

class Move {
public:
    Move() = default;
    explicit Move(chess::Move move) noexcept;

    [[nodiscard]] static Move no_move() noexcept;
    [[nodiscard]] std::string uci() const;
    [[nodiscard]] chess::Move native() const noexcept;

private:
    chess::Move move_{};
};

} // namespace koi
