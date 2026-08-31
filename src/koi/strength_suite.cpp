#include "koi/strength_suite.hpp"

#include <array>

namespace koi {
namespace {

constexpr std::array<StrengthPosition, 7> kStrengthPositions{
    StrengthPosition{
        "mate_in_one",
        "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1",
        "f7e8",
        2,
    },
    StrengthPosition{
        "queen_capture",
        "4k3/8/8/3q4/4Q3/8/8/4K3 w - - 0 1",
        "e4d5",
        2,
    },
    StrengthPosition{
        "promotion",
        "4k3/P7/8/8/8/8/8/4K3 w - - 0 1",
        "a7a8q",
        2,
    },
    StrengthPosition{
        "fork",
        "8/2k1q3/8/8/8/2N5/8/K7 w - - 0 1",
        "c3d5",
        3,
    },
    StrengthPosition{
        "pinned_queen",
        "4k3/4q3/8/8/1B6/8/8/K3R3 w - - 0 1",
        "e1e7",
        2,
    },
    StrengthPosition{
        "recapture",
        "4k3/8/8/8/3p4/4P3/8/4K3 w - - 0 1",
        "e3d4",
        2,
    },
    StrengthPosition{
        "black_mate",
        "8/8/8/8/8/1k6/2q5/K7 b - - 0 1",
        "c2a2",
        2,
    },
};

} // namespace

std::span<const StrengthPosition> strength_positions() noexcept {
    return kStrengthPositions;
}

} // namespace koi
