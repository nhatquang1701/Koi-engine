#pragma once

#include "koi/game_state.hpp"

namespace koi {

class Evaluator {
public:
    virtual ~Evaluator() = default;
    [[nodiscard]] virtual int evaluate(const GameState&, Color perspective) const = 0;
};

} // namespace koi
