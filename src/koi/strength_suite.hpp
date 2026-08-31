#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace koi {

struct StrengthPosition {
    std::string_view name;
    std::string_view fen;
    std::string_view expected_move;
    std::uint8_t depth;
    std::array<std::string_view, 3> accepted_moves{};
    std::string_view category;
    std::optional<int> minimum_score;
    std::uint16_t id = 0;
};

[[nodiscard]] std::span<const StrengthPosition> strength_positions() noexcept;
[[nodiscard]] std::span<const StrengthPosition> optional_strength_positions() noexcept;

} // namespace koi
