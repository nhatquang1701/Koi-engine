#pragma once

#include <cstdint>
#include <random>

#include "koi/move.hpp"

namespace koi {

class Position;

class MoveChooser {
public:
    virtual ~MoveChooser() = default;
    [[nodiscard]] virtual Move choose(const Position& position) = 0;
};

class RandomMoveChooser final : public MoveChooser {
public:
    explicit RandomMoveChooser(std::uint32_t seed);

    void set_seed(std::uint32_t seed);

    [[nodiscard]] Move choose(const Position& position) override;

private:
    std::mt19937 engine_;
};

} // namespace koi
