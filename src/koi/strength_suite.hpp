#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace koi {

struct StrengthPosition {
    std::string_view name;
    std::string_view fen;
    std::string_view expected_move;
    std::uint8_t depth;
};

[[nodiscard]] std::span<const StrengthPosition> strength_positions() noexcept;

} // namespace koi
