#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"

namespace koi {

class Position {
public:
    Position();
    explicit Position(std::string_view fen);

    [[nodiscard]] std::string fen() const;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    bool apply_uci(std::string_view uci);
    bool set_fen(std::string_view fen);

private:
    GameState state_;
};

} // namespace koi
