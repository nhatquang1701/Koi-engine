#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace koi::detail {

inline constexpr int kInfinity = 1'000'000;
inline constexpr int kMateScore = 100'000;
inline constexpr int kMateThreshold = 99'000;
inline constexpr int kMaximumSearchDepth = 64;
inline constexpr int kMaximumQuiescenceDepth = 16;
inline constexpr int kMaximumQuiescenceSafetyDepth = 64;
inline constexpr int kMaximumCheckExtensionsPerPath = 2;
inline constexpr int kMaximumQuiescenceCheckDepth = 3;
inline constexpr int kQuiescenceFutilityMargin = 306;
inline constexpr int kQuiescenceFutilityMoveLimit = 2;
inline constexpr int kQuiescenceSeeThreshold = -74;
inline constexpr int kInternalIterativeReductionMinimumDepth = 6;
inline constexpr int kTranspositionProbCutMargin = 428;
inline constexpr int kNarrowQuietCheckProbeStartDepth = 6;
inline constexpr int kMaximumQuiescenceNarrowQuietCheckDepth = 7;
inline constexpr int kIncompleteRootForcingMargin = 50;
inline constexpr auto kShortTimedFallbackMinimum = std::chrono::milliseconds{150};
inline constexpr auto kShortTimedFallbackThreshold = std::chrono::milliseconds{300};
inline constexpr std::size_t kNullMoveSparsePieceLimit = 8;
inline constexpr std::uint16_t kNullMoveRuleSafetyHalfmoves = 90;

// Kept as a private compatibility predicate for existing diagnostic tests;
// SearchPolicy owns the live LMR decision and uses the same threshold.
[[nodiscard]] constexpr bool high_history_move_excluded_from_lmr(
    const int history_score) noexcept {
    return history_score >= 128;
}

} // namespace koi::detail
