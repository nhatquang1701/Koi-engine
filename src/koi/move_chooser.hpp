#pragma once

#include <cstdint>
#include <random>

#include "koi/game_state.hpp"
#include "koi/move.hpp"

namespace koi {

class MoveChooser {
public:
    virtual ~MoveChooser() = default;
    [[nodiscard]] virtual Move choose(const GameState& state) = 0;
};

class RandomMoveChooser final : public MoveChooser {
public:
    explicit RandomMoveChooser(std::uint32_t seed);

    void set_seed(std::uint32_t seed);

    [[nodiscard]] Move choose(const GameState& state) override;

private:
    std::mt19937 engine_;
};

} // namespace koi
