#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "koi/game_state.hpp"

namespace koi {

inline constexpr std::string_view kClassicalEvaluationFeatureSet = "koi-classical-v1";
inline constexpr std::string_view kNnuePieceSquareV1FeatureSet = "piece-square-v1";
inline constexpr std::string_view kNnuePieceSquareKingPawnV2FeatureSet =
    "piece-square-king-pawn-v2";
inline constexpr std::string_view kNnueHalfkaKingBucketV1FeatureSet =
    "halfka-king-bucket-v1";

inline constexpr std::size_t kNnuePieceSquareV1FeatureCount = 768;
inline constexpr std::size_t kNnueKingContextFeatureCount = 128;
inline constexpr std::size_t kNnuePawnFileContextFeatureCount = 64;
inline constexpr std::size_t kNnuePieceSquareKingPawnV2FeatureCount =
    kNnuePieceSquareV1FeatureCount + kNnueKingContextFeatureCount +
    kNnuePawnFileContextFeatureCount;

// halfka-king-bucket-v1: one 768-input half-king plane block per own-king
// bucket, so 12 buckets times 12 piece planes times 64 squares.  The plane
// order is own pawn..king followed by the opponent pawn..king, relative to the
// side to move.
inline constexpr std::size_t kNnueKingBucketCount = 12;
inline constexpr std::size_t kNnueHalfkaKingBucketPiecePlaneCount = 12;
inline constexpr std::size_t kNnueHalfkaKingBucketV1FeatureCount =
    kNnueKingBucketCount * kNnueHalfkaKingBucketPiecePlaneCount * 64;

using NnueFeatureVectorV1 = std::array<std::int8_t, kNnuePieceSquareV1FeatureCount>;
using NnueFeatureVectorV2 =
    std::array<std::int8_t, kNnuePieceSquareKingPawnV2FeatureCount>;
using NnueFeatureVectorV4 =
    std::array<std::int8_t, kNnueHalfkaKingBucketV1FeatureCount>;

// Sparse form of the v2 input vector: the indices whose dense value is one.
// Inference only has to visit these (about 30 for a typical position) instead
// of scanning all 960 inputs, which is what makes the network usable in a
// search leaf.  The capacity covers the theoretical maximum: 30 non-king
// pieces plus two king contexts plus four pawn-file flags per file and side.
inline constexpr std::size_t kNnueSparseFeatureCapacity = 128;

struct NnueSparseFeatures {
    std::array<std::uint16_t, kNnueSparseFeatureCapacity> indices{};
    std::size_t count = 0;
};

// Sparse form of the halfka-king-bucket-v1 input vector.  A legal position has
// at most 32 active inputs, so 64 entries leave room for unusual positions
// while keeping the per-call structure small.
inline constexpr std::size_t kNnueSparseFeatureCapacityV4 = 64;

struct NnueSparseFeaturesV4 {
    std::array<std::uint16_t, kNnueSparseFeatureCapacityV4> indices{};
    std::size_t count = 0;
};

struct EvaluationFeatures {
    PositionFeatures position{};
    std::uint8_t castling_rights = 0;
};

class EvaluationFeatureExtractor final {
public:
    [[nodiscard]] static EvaluationFeatures extract(const GameState&) noexcept;
    [[nodiscard]] static NnueFeatureVectorV1 encode_piece_square_v1(
        const EvaluationFeatures&) noexcept;
    [[nodiscard]] static NnueFeatureVectorV2 encode_piece_square_king_pawn_v2(
        const EvaluationFeatures&) noexcept;
    [[nodiscard]] static NnueSparseFeatures encode_sparse_v2(
        const EvaluationFeatures&) noexcept;
    [[nodiscard]] static NnueFeatureVectorV4 encode_halfka_king_bucket_v1(
        const EvaluationFeatures&) noexcept;
    [[nodiscard]] static NnueSparseFeaturesV4 encode_sparse_v4(
        const EvaluationFeatures&) noexcept;
    // Perspective-aware variants used by the incremental accumulator: the
    // perspective is a real color, not necessarily the side to move.
    [[nodiscard]] static NnueSparseFeaturesV4 encode_sparse_v4(
        const EvaluationFeatures&, Color perspective) noexcept;
    [[nodiscard]] static std::size_t halfka_king_bucket_for(
        const EvaluationFeatures&, Color perspective) noexcept;
    // Bucket of an explicit own-king square under a real-color perspective.
    // Used by the incremental accumulator, which must know the bucket a king
    // move lands in without rebuilding the whole feature view.
    [[nodiscard]] static std::size_t halfka_king_bucket_for_square(
        Square king_square, Color perspective) noexcept;
    [[nodiscard]] static std::uint16_t halfka_king_bucket_feature_index(
        std::size_t bucket, Color perspective, Piece piece, Square square) noexcept;
};

} // namespace koi
