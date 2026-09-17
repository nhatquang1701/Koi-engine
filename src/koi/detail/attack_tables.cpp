#include "koi/detail/attack_tables.hpp"

#include <array>
#include <bit>

namespace koi::detail {
namespace {

using U64 = std::uint64_t;

// Direction order: N, NE, E, SE, S, SW, W, NW.
constexpr std::array<int, kAttackDirections> kSteps = {8, 9, 1, -7, -8, -9, -1, 7};
constexpr std::array<int, kAttackDirections> kFileDeltas = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr std::array<int, kAttackDirections> kRankDeltas = {1, 1, 0, -1, -1, -1, 0, 1};

[[nodiscard]] constexpr int file_of(int square) noexcept {
    return square & 7;
}

[[nodiscard]] constexpr int rank_of(int square) noexcept {
    return square >> 3;
}

struct Tables {
    std::array<U64, 64> knight{};
    std::array<U64, 64> king{};
    std::array<std::array<U64, 64>, 2> pawn{};
    std::array<std::array<U64, 64>, 2> pawn_attackers{};
    std::array<std::array<U64, 64>, kAttackDirections> rays{};

    Tables() noexcept {
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int rank = rank_of(square);

            // Knights: eight fixed offsets with a two-file / one-rank or
            // one-file / two-rank shape.
            constexpr std::array<std::array<int, 2>, 8> knight_deltas = {{
                {1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}}};
            for (const auto& delta : knight_deltas) {
                const int target_file = file + delta[0];
                const int target_rank = rank + delta[1];
                if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                    knight[square] |= U64{1} << (target_rank * 8 + target_file);
                }
            }

            // Kings: all eight neighbours.
            for (int df = -1; df <= 1; ++df) {
                for (int dr = -1; dr <= 1; ++dr) {
                    if (df == 0 && dr == 0) continue;
                    const int target_file = file + df;
                    const int target_rank = rank + dr;
                    if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                        king[square] |= U64{1} << (target_rank * 8 + target_file);
                    }
                }
            }

            // Pawn pushes: white attacks towards larger ranks, black towards
            // smaller ones.  Both colours capture at file deltas +/-1.
            for (const int df : {-1, 1}) {
                const int white_file = file + df;
                const int white_rank = rank + 1;
                if (white_file >= 0 && white_file < 8 && white_rank < 8) {
                    pawn[0][square] |= U64{1} << (white_rank * 8 + white_file);
                }
                const int black_file = file + df;
                const int black_rank = rank - 1;
                if (black_file >= 0 && black_file < 8 && black_rank >= 0) {
                    pawn[1][square] |= U64{1} << (black_rank * 8 + black_file);
                }
            }

            // Reverse tables: which squares can a pawn of a colour attack
            // `square` from?  A white pawn attacks upwards, so its sources are
            // one rank below with mirrored file deltas.
            for (const int df : {-1, 1}) {
                const int white_file = file + df;
                const int white_rank = rank - 1;
                if (white_file >= 0 && white_file < 8 && white_rank >= 0) {
                    pawn_attackers[0][square] |= U64{1} << (white_rank * 8 + white_file);
                }
                const int black_file = file + df;
                const int black_rank = rank + 1;
                if (black_file >= 0 && black_file < 8 && black_rank < 8) {
                    pawn_attackers[1][square] |= U64{1} << (black_rank * 8 + black_file);
                }
            }

            // Rays: walk each direction until the edge of the board.
            for (int direction = 0; direction < kAttackDirections; ++direction) {
                const std::size_t index = static_cast<std::size_t>(direction);
                const int file_delta = kFileDeltas[index];
                const int rank_delta = kRankDeltas[index];
                int target_file = file + file_delta;
                int target_rank = rank + rank_delta;
                U64 mask = 0;
                while (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                    mask |= U64{1} << (target_rank * 8 + target_file);
                    target_file += file_delta;
                    target_rank += rank_delta;
                }
                rays[index][static_cast<std::size_t>(square)] = mask;
            }
        }
    }
};

[[nodiscard]] const Tables& tables() noexcept {
    static const Tables instance{};
    return instance;
}

[[nodiscard]] std::uint64_t sliding_attacks(int square, std::uint64_t occupied,
                                            std::initializer_list<int> directions) noexcept {
    if (square < 0 || square >= 64) return 0;
    const Tables& t = tables();
    U64 attacks = 0;
    for (const int direction : directions) {
        const std::size_t index = static_cast<std::size_t>(direction);
        const U64 ray = t.rays[index][static_cast<std::size_t>(square)];
        const U64 blockers = ray & occupied;
        if (blockers == 0) {
            attacks |= ray;
            continue;
        }
        // The classical trick: the blocker's own ray is a subset of this ray,
        // so XOR removes everything beyond the first occupied square.
        const int step = kSteps[index];
        const int first = step > 0 ? std::countr_zero(blockers)
                                   : 63 - std::countl_zero(blockers);
        attacks |= ray ^ t.rays[index][static_cast<std::size_t>(first)];
    }
    return attacks;
}

} // namespace

std::uint64_t knight_attacks(int square) noexcept {
    if (square < 0 || square >= 64) return 0;
    return tables().knight[static_cast<std::size_t>(square)];
}

std::uint64_t king_attacks(int square) noexcept {
    if (square < 0 || square >= 64) return 0;
    return tables().king[static_cast<std::size_t>(square)];
}

std::uint64_t pawn_attacks(int square, bool white) noexcept {
    if (square < 0 || square >= 64) return 0;
    return tables().pawn[white ? 0 : 1][static_cast<std::size_t>(square)];
}

std::uint64_t pawn_attackers_of(int target, bool white_pawns) noexcept {
    if (target < 0 || target >= 64) return 0;
    return tables().pawn_attackers[white_pawns ? 0 : 1][static_cast<std::size_t>(target)];
}

std::uint64_t bishop_attacks(int square, std::uint64_t occupied) noexcept {
    return sliding_attacks(square, occupied, {1, 3, 5, 7});
}

std::uint64_t rook_attacks(int square, std::uint64_t occupied) noexcept {
    return sliding_attacks(square, occupied, {0, 2, 4, 6});
}

std::uint64_t queen_attacks(int square, std::uint64_t occupied) noexcept {
    return sliding_attacks(square, occupied, {0, 1, 2, 3, 4, 5, 6, 7});
}

std::uint64_t ray_attacks(int square, int direction) noexcept {
    if (square < 0 || square >= 64 || direction < 0 || direction >= kAttackDirections) return 0;
    return tables().rays[static_cast<std::size_t>(direction)][static_cast<std::size_t>(square)];
}

int nearest_blocker(int square, int direction, std::uint64_t occupied) noexcept {
    const U64 ray = ray_attacks(square, direction);
    if (ray == 0) return -1;
    const U64 blockers = ray & occupied;
    if (blockers == 0) return -1;
    const int step = kSteps[static_cast<std::size_t>(direction)];
    return step > 0 ? std::countr_zero(blockers) : 63 - std::countl_zero(blockers);
}

} // namespace koi::detail
