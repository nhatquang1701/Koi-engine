#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "koi/detail/search_stack.hpp"

namespace koi::detail {

struct RootLine {
    bool completed = false;
    // `completed` means the root move was searched to a usable return value;
    // `exact` is stricter and excludes scout-only or selective-bound scores.
    // Root ranking may use a completed selective estimate as a fallback/order
    // hint, but public completed-depth and aspiration authority require `exact`.
    bool exact = false;
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
