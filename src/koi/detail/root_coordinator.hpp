#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "koi/detail/search_stack.hpp"

namespace koi::detail {

struct RootLine {
    // A line can be searched without producing a comparable score: a
    // null-window scout may fail low, or a selective child may return an
    // unresolved bound. Keep coverage separate from ranking eligibility so a
    // dropped line cannot silently make the remaining incumbent authoritative.
    bool searched = false;
    bool completed = false;
    // `completed` means the root move was searched to a usable return value;
    // `exact` is stricter and excludes scout-only or selective-bound scores.
    // Root ranking may use a completed selective estimate as a fallback/order
    // hint. Public completed-depth publication requires complete root
    // coverage and a usable line; strict aspiration/proof authority requires
    // `exact` provenance separately.
    bool exact = false;
    // An inexact/selective child is safe to omit from the comparable ranking
    // only when its returned score is a fail-low upper bound at the window
    // that bounded that root move. An unresolved selective score may still be
    // stronger than the ranked incumbent and therefore blocks root authority.
    bool selective_bound = false;
    bool safe_upper_bound = false;
    // The child returned a lower bound before negation, so this root score is
    // an upper bound. It may become safe after a later exact incumbent raises
    // the root floor above the score, even when it was the first move searched.
    bool selective_upper_bound = false;
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
