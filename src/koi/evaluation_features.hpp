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

inline constexpr std::size_t kNnuePieceSquareV1FeatureCount = 768;
inline constexpr std::size_t kNnueKingContextFeatureCount = 128;
inline constexpr std::size_t kNnuePawnFileContextFeatureCount = 64;
inline constexpr std::size_t kNnuePieceSquareKingPawnV2FeatureCount =
    kNnuePieceSquareV1FeatureCount + kNnueKingContextFeatureCount +
    kNnuePawnFileContextFeatureCount;

using NnueFeatureVectorV1 = std::array<std::int8_t, kNnuePieceSquareV1FeatureCount>;
using NnueFeatureVectorV2 =
    std::array<std::int8_t, kNnuePieceSquareKingPawnV2FeatureCount>;

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
};

} // namespace koi
