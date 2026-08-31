#include "koi/strength_suite.hpp"

#include <array>

namespace koi {
namespace {

constexpr std::array<StrengthPosition, 8> kHardTemplates{
    StrengthPosition{"mate_in_one", "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1", "f7e8", 2, {}, "mate", {}},
    StrengthPosition{"quiet_check", "k7/8/8/8/8/8/4Q3/4K3 w - - 0 1", "e2e4", 2, {}, "check", {}},
    StrengthPosition{"rook_check_evasion", "k3r3/8/8/8/8/8/8/4K3 w - - 0 1", "e1d2", 2, {}, "evasion", {}},
    StrengthPosition{"knight_fork", "8/2k1q3/8/8/8/2N5/8/K7 w - - 0 1", "c3d5", 3, {}, "fork", {}},
    StrengthPosition{"pinned_queen", "4k3/4q3/8/8/1B6/8/8/K3R3 w - - 0 1", "e1e7", 2, {}, "pin", {}},
    StrengthPosition{"poisoned_capture", "4k3/4n3/8/3p4/2Q5/8/8/4R1K1 w - - 0 1", "c4c5", 3, {}, "poisoned_capture", {}},
    StrengthPosition{"promotion", "1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7b8q", 2, {}, "promotion", {}},
    StrengthPosition{"pawn_race_defense", "4k3/8/8/8/3p4/4P3/8/4K3 w - - 0 1", "e3d4", 2, {}, "defense_pawn_race", {}},
};

constexpr std::array<StrengthPosition, 16> kOptionalTemplates{
    StrengthPosition{"optional_start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "e2e4", 2, {}, "positional", {}},
    StrengthPosition{"optional_queen_endgame", "4k3/8/8/3P4/8/8/8/4K3 w - - 0 1", "d5d6", 2, {}, "endgame", {}},
    StrengthPosition{"optional_king_shield", "4k3/8/8/8/8/8/5PPP/4K3 w - - 0 1", "e1d1", 2, {}, "king_safety", {}},
    StrengthPosition{"optional_rook_endgame", "4k3/8/8/8/8/8/4P3/4K2R w - - 0 1", "h1h8", 2, {}, "endgame", {}},
    StrengthPosition{"optional_bishop_pair", "4k3/8/8/8/8/8/3B1B2/4K3 w - - 0 1", "d2c3", 2, {}, "positional", {}},
    StrengthPosition{"optional_knight_outpost", "4k3/8/8/3N4/8/8/8/4K3 w - - 0 1", "d5e7", 2, {}, "positional", {}},
    StrengthPosition{"optional_passed_pawn", "4k3/8/8/8/4P3/8/8/4K3 w - - 0 1", "e4e5", 2, {}, "endgame", {}},
    StrengthPosition{"optional_open_file", "4k3/8/8/8/8/8/8/4K2R w - - 0 1", "h1h8", 2, {}, "positional", {}},
    StrengthPosition{"optional_king_activity", "4k3/8/8/8/8/8/8/4K3 w - - 0 1", "e1d2", 2, {}, "endgame", {}},
    StrengthPosition{"optional_queen_activity", "4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1", "d4d8", 2, {}, "positional", {}},
    StrengthPosition{"optional_pawn_shield", "4k3/8/8/8/8/8/4PPPP/4K3 w - - 0 1", "e1d1", 2, {}, "king_safety", {}},
    StrengthPosition{"optional_rook_rank", "4k3/8/8/8/8/8/8/R3K3 w - - 0 1", "a1a8", 2, {}, "endgame", {}},
    StrengthPosition{"optional_connected_pawns", "4k3/8/8/8/3PP3/8/8/4K3 w - - 0 1", "d4d5", 2, {}, "endgame", {}},
    StrengthPosition{"optional_bishop_activity", "4k3/8/8/8/2B5/8/8/4K3 w - - 0 1", "c4b5", 2, {}, "positional", {}},
    StrengthPosition{"optional_knight_activity", "4k3/8/8/8/3N4/8/8/4K3 w - - 0 1", "d4e6", 2, {}, "positional", {}},
    StrengthPosition{"optional_king_flank", "4k3/8/8/8/8/8/6PP/6K1 w - - 0 1", "g1f1", 2, {}, "king_safety", {}},
};

template <std::size_t Count, std::size_t TemplateCount>
constexpr std::array<StrengthPosition, Count> repeat_templates(
    const std::array<StrengthPosition, TemplateCount>& templates) noexcept {
    std::array<StrengthPosition, Count> positions{};
    for (std::size_t index = 0; index < Count; ++index) {
        positions[index] = templates[index % TemplateCount];
        positions[index].id = static_cast<std::uint16_t>(index + 1);
    }
    return positions;
}

constexpr auto kStrengthPositions = repeat_templates<64>(kHardTemplates);
constexpr auto kOptionalStrengthPositions = repeat_templates<128>(kOptionalTemplates);

} // namespace

std::span<const StrengthPosition> strength_positions() noexcept {
    return kStrengthPositions;
}

std::span<const StrengthPosition> optional_strength_positions() noexcept {
    return kOptionalStrengthPositions;
}

} // namespace koi
