#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <chess.hpp>

#include "koi/move.hpp"

namespace koi {

class Position {
public:
    Position();
    explicit Position(std::string_view fen);

    [[nodiscard]] std::string fen() const;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    bool apply_uci(std::string_view uci);
    bool set_fen(std::string_view fen);
    [[nodiscard]] const chess::Board& board() const noexcept;

private:
    chess::Board board_{};
    std::optional<std::string> fen_override_;
};

} // namespace koi
