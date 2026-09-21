#pragma once

#include <array>

namespace koi {
namespace detail {

inline constexpr std::array<int, 64> kPawnMiddleGame{
      0,   0,   0,   0,   0,   0,   0,   0,
      5,  10,  10, -20, -20,  10,  10,   5,
      5,  -5, -10,   0,   0, -10,  -5,   5,
      0,   0,   0,  20,  20,   0,   0,   0,
      5,   5,  10,  25,  25,  10,   5,   5,
     10,  10,  20,  30,  30,  20,  10,  10,
     50,  50,  50,  50,  50,  50,  50,  50,
      0,   0,   0,   0,   0,   0,   0,   0,
};

inline constexpr std::array<int, 64> kPawnEndGame{
      0,   0,   0,   0,   0,   0,   0,   0,
     10,  12,  14,  16,  16,  14,  12,  10,
     10,  12,  14,  18,  18,  14,  12,  10,
     12,  14,  18,  24,  24,  18,  14,  12,
     16,  18,  24,  32,  32,  24,  18,  16,
     22,  24,  30,  40,  40,  30,  24,  22,
     36,  38,  44,  52,  52,  44,  38,  36,
      0,   0,   0,   0,   0,   0,   0,   0,
};

inline constexpr std::array<int, 64> kKnightMiddleGame{
    -50, -40, -30, -30, -30, -30, -40, -50,
    -40, -20,   0,   0,   0,   0, -20, -40,
    -30,   0,  10,  15,  15,  10,   0, -30,
    -30,   5,  15,  20,  20,  15,   5, -30,
    -30,   0,  15,  20,  20,  15,   0, -30,
    -30,   5,  10,  15,  15,  10,   5, -30,
    -40, -20,   0,   5,   5,   0, -20, -40,
    -50, -40, -30, -30, -30, -30, -40, -50,
};

inline constexpr std::array<int, 64> kKnightEndGame{
    -35, -25, -15, -10, -10, -15, -25, -35,
    -25, -10,   5,  10,  10,   5, -10, -25,
    -15,   5,  15,  20,  20,  15,   5, -15,
    -10,  10,  20,  25,  25,  20,  10, -10,
    -10,  10,  20,  25,  25,  20,  10, -10,
    -15,   5,  15,  20,  20,  15,   5, -15,
    -25, -10,   5,  10,  10,   5, -10, -25,
    -35, -25, -15, -10, -10, -15, -25, -35,
};

inline constexpr std::array<int, 64> kBishopMiddleGame{
    -20, -10, -10, -10, -10, -10, -10, -20,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -10,   0,   5,  10,  10,   5,   0, -10,
    -10,   5,   5,  10,  10,   5,   5, -10,
    -10,   0,  10,  10,  10,  10,   0, -10,
    -10,  10,  10,  10,  10,  10,  10, -10,
    -10,   5,   0,   0,   0,   0,   5, -10,
    -20, -10, -10, -10, -10, -10, -10, -20,
};

inline constexpr std::array<int, 64> kBishopEndGame{
    -15, -10,  -8,  -6,  -6,  -8, -10, -15,
     -8,   0,   4,   8,   8,   4,   0,  -8,
     -6,   4,   9,  12,  12,   9,   4,  -6,
     -4,   8,  12,  16,  16,  12,   8,  -4,
     -4,   8,  12,  16,  16,  12,   8,  -4,
     -6,   4,   9,  12,  12,   9,   4,  -6,
     -8,   0,   4,   8,   8,   4,   0,  -8,
    -15, -10,  -8,  -6,  -6,  -8, -10, -15,
};

inline constexpr std::array<int, 64> kRookMiddleGame{
      0,   0,   0,   0,   0,   0,   0,   0,
      5,  10,  10,  10,  10,  10,  10,   5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
      0,   0,   5,  10,  10,   5,   0,   0,
};

inline constexpr std::array<int, 64> kRookEndGame{
      0,   0,   5,  10,  10,   5,   0,   0,
      5,  10,  15,  20,  20,  15,  10,   5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
      5,  10,  15,  20,  20,  15,  10,   5,
      0,   0,   5,  10,  10,   5,   0,   0,
};

inline constexpr std::array<int, 64> kQueenMiddleGame{
    -20, -10, -10,  -5,  -5, -10, -10, -20,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -10,   0,   5,   5,   5,   5,   0, -10,
     -5,   0,   5,   5,   5,   5,   0,  -5,
      0,   0,   5,   5,   5,   5,   0,  -5,
    -10,   5,   5,   5,   5,   5,   0, -10,
    -10,   0,   5,   0,   0,   0,   0, -10,
    -20, -10, -10,  -5,  -5, -10, -10, -20,
};

inline constexpr std::array<int, 64> kQueenEndGame{
    -10,  -5,  -5,   0,   0,  -5,  -5, -10,
     -5,   0,   5,   5,   5,   5,   0,  -5,
     -5,   5,  10,  12,  12,  10,   5,  -5,
      0,   5,  12,  15,  15,  12,   5,   0,
      0,   5,  12,  15,  15,  12,   5,   0,
     -5,   5,  10,  12,  12,  10,   5,  -5,
     -5,   0,   5,   5,   5,   5,   0,  -5,
    -10,  -5,  -5,   0,   0,  -5,  -5, -10,
};

inline constexpr std::array<int, 64> kKingMiddleGame{
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -10, -20, -20, -20, -20, -20, -20, -10,
     20,  20,   0,   0,   0,   0,  20,  20,
     20,  30,  10,   0,   0,  10,  30,  20,
};

inline constexpr std::array<int, 64> kKingEndGame{
    -50, -40, -30, -20, -20, -30, -40, -50,
    -30, -20, -10,   0,   0, -10, -20, -30,
    -20, -10,  10,  20,  20,  10, -10, -20,
    -10,   0,  20,  30,  30,  20,   0, -10,
    -10,   0,  20,  30,  30,  20,   0, -10,
    -20, -10,  10,  20,  20,  10, -10, -20,
    -30, -20, -10,   0,   0, -10, -20, -30,
    -50, -40, -30, -20, -20, -30, -40, -50,
};

}  // namespace detail
}  // namespace koi

// A tuned table set is optional: `tools/measurement/tune_classical.py` can
// generate `koi/evaluation_piece_squares_tuned.hpp` from a labeled corpus, and
// the evaluator adds those per-square corrections to the built-in tables.  A
// build without the generated header uses the built-in values unchanged.
#if __has_include("koi/evaluation_piece_squares_tuned.hpp")
#include "koi/evaluation_piece_squares_tuned.hpp"
#else
namespace koi {
namespace detail {

inline constexpr bool kHasTunedPieceSquares = false;
inline constexpr std::array<int, 6 * 64> kTunedMiddleGameDeltas{};
inline constexpr std::array<int, 6 * 64> kTunedEndGameDeltas{};

}  // namespace detail
}  // namespace koi
#endif
