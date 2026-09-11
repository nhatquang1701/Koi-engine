#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "koi/detail/search_stack.hpp"

namespace koi::detail {

struct RootLine {
    bool completed = false;
    int score = -1'000'000;
    std::size_t stable_index = 0;
    PrincipalVariation pv;
};

class RootCoordinator {
public:
    [[nodiscard]] static std::vector<std::size_t>
    rank(const std::vector<RootLine>& lines);

    [[nodiscard]] static std::optional<std::size_t>
    best_completed(const std::vector<RootLine>& lines);
};

} // namespace koi::detail
