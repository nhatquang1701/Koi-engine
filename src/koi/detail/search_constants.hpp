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
// Checked qsearch uses native recursion, and every frame owns a fixed move
// metadata buffer.  Keep the safety horizon materially below the regular
// search-stack capacity so a perpetual checking sequence cannot exhaust the
// process stack before the boundary is observed.
inline constexpr int kMaximumQuiescenceSafetyDepth = 24;
inline constexpr int kMaximumCheckExtensionsPerPath = 2;
inline constexpr int kMaximumQuiescenceCheckDepth = 3;
inline constexpr int kQuiescenceFutilityMargin = 306;
inline constexpr int kQuiescenceFutilityMoveLimit = 2;
// Only captures whose static exchange value is worse than this floor are
// removed from the tactical frontier.  A floor of zero discards the sound
// defensive captures in the -1..-73 band that perpetual-check and sacrifice
// resources live in, so the boundary is the reference -74 instead.
inline constexpr int kQuiescenceSeeThreshold = -74;
// Fail-high qsearch returns are pulled toward beta so a large overshoot does
// not destabilise the parent's window.  The weights are the reference
// fixed-point blend: score * weight + beta * (1024 - weight), over 1024.
// Decisive scores are never softened.
inline constexpr int kQuiescenceStandPatSofteningWeight = 441;
inline constexpr int kQuiescenceFailHighSofteningWeight = 462;
inline constexpr int kInternalIterativeReductionMinimumDepth = 6;
// True internal iterative deepening re-searches the current node at a reduced
// depth with a null window when no transposition move is available, so the
// full-depth move loop starts from a table-seeded ordering.  The minimum
// depth keeps the probe off shallow nodes where the extra search costs more
// than the ordering it buys.
inline constexpr int kTrueInternalIterativeDeepeningMinimumDepth = 6;
inline constexpr int kTranspositionProbCutMargin = 428;
inline constexpr int kReverseFutilityMinimumDepth = 2;
inline constexpr int kReverseFutilityMaximumDepth = 12;
inline constexpr int kReverseFutilityBaseMargin = 80;
inline constexpr int kReverseFutilityDepthMargin = 60;
inline constexpr int kReverseFutilityImprovingDiscount = 32;
inline constexpr int kReverseFutilityWorseningSurcharge = 24;
// A continuation-history pruning threshold adapted to Koi's signed history
// range.  Keep it materially below the neutral initialization so only a
// repeatedly failing move is removed from a late quiet scout node.
inline constexpr int kNegativeContinuationHistoryBase = 4'136;
inline constexpr int kNegativeContinuationHistoryMinimumDepth = 4;
inline constexpr int kNegativeContinuationHistoryMinimumMove = 3;
inline constexpr int kNarrowQuietCheckProbeStartDepth = 6;
inline constexpr int kMaximumQuiescenceNarrowQuietCheckDepth = 7;
inline constexpr int kMaximumQuiescenceNarrowQuietChecks = 2;
inline constexpr int kIncompleteRootForcingMargin = 50;
inline constexpr auto kShortTimedFallbackMinimum = std::chrono::milliseconds{150};
inline constexpr auto kShortTimedFallbackThreshold = std::chrono::milliseconds{300};
// Root workers have enough startup/coordination cost that a forcing root with
// roughly half a second of requested time needs a bounded first qsearch pass
// to finish its serial root iteration.  This is a timing-policy boundary, not
// a change to fixed-depth or ordinary long-search qsearch.
inline constexpr auto kShortTimedSerialThreshold = std::chrono::milliseconds{500};
inline constexpr std::size_t kNullMoveSparsePieceLimit = 8;
inline constexpr std::uint16_t kNullMoveRuleSafetyHalfmoves = 90;
// Koi's classical evaluator has a substantially narrower positional-score
// distribution than the reference NNUE search.  The null fail-high is still
// verified before it can cut off, so use a smaller static confidence margin
// and let depth supply the remaining safety distance.
inline constexpr int kNullMoveStaticMargin = 96;
// Null-move verification is a deep-node safety net.  Below this depth an
// eligible null fail-high is accepted when the reduced probe itself reached
// beta without a selective cutoff; at and above it, a no-null, no-TT
// confirmation search must reproduce the fail-high before the cutoff stands.
inline constexpr int kNullMoveVerificationMinimumDepth = 16;

// Interior Syzygy WDL cutoffs are decisive but deliberately below the mate
// threshold: a tablebase win proves the result, not a mate distance, so it
// must never be advertised as a mate score.  The magnitude still dominates
// any static evaluation.
inline constexpr int kTablebaseInteriorWinScore = 90'000;
inline constexpr int kTablebaseInteriorLossScore = -90'000;
// Interior probing starts at or below this remaining depth when enabled.
inline constexpr int kMinimumSyzygyInteriorDepth = 1;

// Kept as a private compatibility predicate for existing diagnostic tests;
// SearchPolicy owns the live LMR decision and uses the same threshold.
[[nodiscard]] constexpr bool high_history_move_excluded_from_lmr(
    const int history_score) noexcept {
    return history_score >= 128;
}

} // namespace koi::detail
