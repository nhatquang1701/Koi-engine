#include "koi/detail/root_coordinator.hpp"

#include <algorithm>

namespace koi::detail {

std::vector<std::size_t> RootCoordinator::rank(const std::vector<RootLine>& lines) {
    std::vector<std::size_t> ranked;
    ranked.reserve(lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].completed) {
            ranked.push_back(index);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [&lines](const std::size_t left,
                                                      const std::size_t right) {
        if (lines[left].score != lines[right].score) {
            return lines[left].score > lines[right].score;
        }
        return lines[left].stable_index < lines[right].stable_index;
    });
    return ranked;
}

std::optional<std::size_t> RootCoordinator::best_completed(
    const std::vector<RootLine>& lines) {
    const std::vector<std::size_t> ranked = rank(lines);
    if (ranked.empty()) {
        return std::nullopt;
    }
    return ranked.front();
}

} // namespace koi::detail
