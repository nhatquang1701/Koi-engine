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

    constexpr StrengthPosition(std::uint16_t fixture_id, std::string_view fixture_name,
                               std::string_view fixture_fen, std::string_view fixture_expected_move,
                               std::uint8_t fixture_depth,
                               std::array<std::string_view, 3> fixture_accepted_moves,
                               std::string_view fixture_category,
                               std::optional<int> fixture_minimum_score = {}) noexcept
        : name(fixture_name), fen(fixture_fen), expected_move(fixture_expected_move),
          depth(fixture_depth), accepted_moves(fixture_accepted_moves), category(fixture_category),
          minimum_score(fixture_minimum_score), id(fixture_id) {}

    constexpr StrengthPosition(std::string_view fixture_name, std::string_view fixture_fen,
                               std::string_view fixture_expected_move,
                               std::uint8_t fixture_depth) noexcept
        : name(fixture_name), fen(fixture_fen), expected_move(fixture_expected_move),
          depth(fixture_depth) {}

    constexpr StrengthPosition(std::string_view fixture_name, std::string_view fixture_fen,
                               std::string_view fixture_expected_move,
                               std::uint8_t fixture_depth,
                               std::array<std::string_view, 3> fixture_accepted_moves,
                               std::string_view fixture_category,
                               std::optional<int> fixture_minimum_score,
                               std::uint16_t fixture_id) noexcept
        : name(fixture_name), fen(fixture_fen), expected_move(fixture_expected_move),
          depth(fixture_depth), accepted_moves(fixture_accepted_moves), category(fixture_category),
          minimum_score(fixture_minimum_score), id(fixture_id) {}
};

[[nodiscard]] std::span<const StrengthPosition> strength_positions() noexcept;
[[nodiscard]] std::span<const StrengthPosition> optional_strength_positions() noexcept;

} // namespace koi
