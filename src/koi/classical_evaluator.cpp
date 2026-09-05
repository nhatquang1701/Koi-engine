#include "koi/classical_evaluator.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>

namespace koi {
namespace {

constexpr const auto& kEvaluation = kClassicalEvaluationParameters;

constexpr int piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return kEvaluation.pawn_value;
    case PieceType::knight:
        return kEvaluation.knight_value;
    case PieceType::bishop:
        return kEvaluation.bishop_value;
    case PieceType::rook:
        return kEvaluation.rook_value;
    case PieceType::queen:
        return kEvaluation.queen_value;
    case PieceType::king:
    case PieceType::none:
        return 0;
    }
    return 0;
}

constexpr std::array<int, 64> kPawnMiddleGame{
      0,   0,   0,   0,   0,   0,   0,   0,
      5,  10,  10, -20, -20,  10,  10,   5,
      5,  -5, -10,   0,   0, -10,  -5,   5,
      0,   0,   0,  20,  20,   0,   0,   0,
      5,   5,  10,  25,  25,  10,   5,   5,
     10,  10,  20,  30,  30,  20,  10,  10,
     50,  50,  50,  50,  50,  50,  50,  50,
      0,   0,   0,   0,   0,   0,   0,   0,
};

constexpr std::array<int, 64> kPawnEndGame{
      0,   0,   0,   0,   0,   0,   0,   0,
     10,  12,  14,  16,  16,  14,  12,  10,
     10,  12,  14,  18,  18,  14,  12,  10,
     12,  14,  18,  24,  24,  18,  14,  12,
     16,  18,  24,  32,  32,  24,  18,  16,
     22,  24,  30,  40,  40,  30,  24,  22,
     36,  38,  44,  52,  52,  44,  38,  36,
      0,   0,   0,   0,   0,   0,   0,   0,
};

constexpr std::array<int, 64> kKnightMiddleGame{
    -50, -40, -30, -30, -30, -30, -40, -50,
    -40, -20,   0,   0,   0,   0, -20, -40,
    -30,   0,  10,  15,  15,  10,   0, -30,
    -30,   5,  15,  20,  20,  15,   5, -30,
    -30,   0,  15,  20,  20,  15,   0, -30,
    -30,   5,  10,  15,  15,  10,   5, -30,
    -40, -20,   0,   5,   5,   0, -20, -40,
    -50, -40, -30, -30, -30, -30, -40, -50,
};

constexpr std::array<int, 64> kKnightEndGame{
    -35, -25, -15, -10, -10, -15, -25, -35,
    -25, -10,   5,  10,  10,   5, -10, -25,
    -15,   5,  15,  20,  20,  15,   5, -15,
    -10,  10,  20,  25,  25,  20,  10, -10,
    -10,  10,  20,  25,  25,  20,  10, -10,
    -15,   5,  15,  20,  20,  15,   5, -15,
    -25, -10,   5,  10,  10,   5, -10, -25,
    -35, -25, -15, -10, -10, -15, -25, -35,
};

constexpr std::array<int, 64> kBishopMiddleGame{
    -20, -10, -10, -10, -10, -10, -10, -20,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -10,   0,   5,  10,  10,   5,   0, -10,
    -10,   5,   5,  10,  10,   5,   5, -10,
    -10,   0,  10,  10,  10,  10,   0, -10,
    -10,  10,  10,  10,  10,  10,  10, -10,
    -10,   5,   0,   0,   0,   0,   5, -10,
    -20, -10, -10, -10, -10, -10, -10, -20,
};

constexpr std::array<int, 64> kBishopEndGame{
    -15, -10,  -8,  -6,  -6,  -8, -10, -15,
     -8,   0,   4,   8,   8,   4,   0,  -8,
     -6,   4,   9,  12,  12,   9,   4,  -6,
     -4,   8,  12,  16,  16,  12,   8,  -4,
     -4,   8,  12,  16,  16,  12,   8,  -4,
     -6,   4,   9,  12,  12,   9,   4,  -6,
     -8,   0,   4,   8,   8,   4,   0,  -8,
    -15, -10,  -8,  -6,  -6,  -8, -10, -15,
};

constexpr std::array<int, 64> kRookMiddleGame{
      0,   0,   0,   0,   0,   0,   0,   0,
      5,  10,  10,  10,  10,  10,  10,   5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
      0,   0,   5,  10,  10,   5,   0,   0,
};

constexpr std::array<int, 64> kRookEndGame{
      0,   0,   5,  10,  10,   5,   0,   0,
      5,  10,  15,  20,  20,  15,  10,   5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
     -5,   0,   5,  10,  10,   5,   0,  -5,
      5,  10,  15,  20,  20,  15,  10,   5,
      0,   0,   5,  10,  10,   5,   0,   0,
};

constexpr std::array<int, 64> kQueenMiddleGame{
    -20, -10, -10,  -5,  -5, -10, -10, -20,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -10,   0,   5,   5,   5,   5,   0, -10,
     -5,   0,   5,   5,   5,   5,   0,  -5,
      0,   0,   5,   5,   5,   5,   0,  -5,
    -10,   5,   5,   5,   5,   5,   0, -10,
    -10,   0,   5,   0,   0,   0,   0, -10,
    -20, -10, -10,  -5,  -5, -10, -10, -20,
};

constexpr std::array<int, 64> kQueenEndGame{
    -10,  -5,  -5,   0,   0,  -5,  -5, -10,
     -5,   0,   5,   5,   5,   5,   0,  -5,
     -5,   5,  10,  12,  12,  10,   5,  -5,
      0,   5,  12,  15,  15,  12,   5,   0,
      0,   5,  12,  15,  15,  12,   5,   0,
     -5,   5,  10,  12,  12,  10,   5,  -5,
     -5,   0,   5,   5,   5,   5,   0,  -5,
    -10,  -5,  -5,   0,   0,  -5,  -5, -10,
};

constexpr std::array<int, 64> kKingMiddleGame{
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -10, -20, -20, -20, -20, -20, -20, -10,
     20,  20,   0,   0,   0,   0,  20,  20,
     20,  30,  10,   0,   0,  10,  30,  20,
};

constexpr std::array<int, 64> kKingEndGame{
    -50, -40, -30, -20, -20, -30, -40, -50,
    -30, -20, -10,   0,   0, -10, -20, -30,
    -20, -10,  10,  20,  20,  10, -10, -20,
    -10,   0,  20,  30,  30,  20,   0, -10,
    -10,   0,  20,  30,  30,  20,   0, -10,
    -20, -10,  10,  20,  20,  10, -10, -20,
    -30, -20, -10,   0,   0, -10, -20, -30,
    -50, -40, -30, -20, -20, -30, -40, -50,
};

using PieceSquareTable = std::array<int, 64>;

const PieceSquareTable& middle_game_table(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn: return kPawnMiddleGame;
    case PieceType::knight: return kKnightMiddleGame;
    case PieceType::bishop: return kBishopMiddleGame;
    case PieceType::rook: return kRookMiddleGame;
    case PieceType::queen: return kQueenMiddleGame;
    case PieceType::king: return kKingMiddleGame;
    case PieceType::none: return kPawnMiddleGame;
    }
    return kPawnMiddleGame;
}

const PieceSquareTable& end_game_table(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn: return kPawnEndGame;
    case PieceType::knight: return kKnightEndGame;
    case PieceType::bishop: return kBishopEndGame;
    case PieceType::rook: return kRookEndGame;
    case PieceType::queen: return kQueenEndGame;
    case PieceType::king: return kKingEndGame;
    case PieceType::none: return kPawnEndGame;
    }
    return kPawnEndGame;
}

constexpr int color_index(Color color) noexcept {
    return color == Color::white ? 0 : 1;
}

constexpr std::uint8_t mirrored_square(std::uint8_t square) noexcept {
    return static_cast<std::uint8_t>((7 - square / 8) * 8 + square % 8);
}

constexpr bool inside(int file, int rank) noexcept {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

std::uint64_t king_zone(std::uint8_t square) noexcept {
    const int file = square % 8;
    const int rank = square / 8;
    std::uint64_t zone = 0;
    for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
        for (int file_delta = -1; file_delta <= 1; ++file_delta) {
            const int target_file = file + file_delta;
            const int target_rank = rank + rank_delta;
            if (inside(target_file, target_rank)) {
                zone |= std::uint64_t{1} << (target_rank * 8 + target_file);
            }
        }
    }
    return zone;
}

bool insufficient_material(const PositionFeatures& features) noexcept {
    int minor_count = 0;
    bool only_bishops = true;
    int bishop_complex = -1;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        switch (piece.type) {
        case PieceType::pawn:
        case PieceType::rook:
        case PieceType::queen:
            return false;
        case PieceType::knight:
            ++minor_count;
            only_bishops = false;
            break;
        case PieceType::bishop: {
            ++minor_count;
            const int complex = (square % 8 + square / 8) & 1;
            if (bishop_complex == -1) {
                bishop_complex = complex;
            } else if (bishop_complex != complex) {
                return false;
            }
            break;
        }
        case PieceType::king:
        case PieceType::none:
            break;
        }
    }

    if (minor_count == 0 || minor_count == 1) {
        return true;
    }
    return only_bishops && bishop_complex != -1;
}

int sliding_mobility(const PositionFeatures& features, std::uint8_t square,
                     PieceType type) noexcept {
    constexpr int bishop_directions[4][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    constexpr int queen_directions[8][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    };
    const int (*directions)[2] = type == PieceType::bishop ? bishop_directions : queen_directions;
    const int direction_count = type == PieceType::bishop ? 4 : 8;
    const Piece moving = features.board[square];
    const int file = square % 8;
    const int rank = square / 8;
    int mobility = 0;

    for (int direction = 0; direction < direction_count; ++direction) {
        int target_file = file + directions[direction][0];
        int target_rank = rank + directions[direction][1];
        while (inside(target_file, target_rank)) {
            const Piece target = features.board[static_cast<std::size_t>(target_rank * 8 + target_file)];
            if (target.empty()) {
                ++mobility;
            } else {
                if (target.color != moving.color) {
                    ++mobility;
                }
                break;
            }
            target_file += directions[direction][0];
            target_rank += directions[direction][1];
        }
    }
    return mobility;
}

int tapered_piece_square(PieceType type, std::uint8_t square, int phase) noexcept {
    const int middle = middle_game_table(type)[square];
    const int end = end_game_table(type)[square];
    return (middle * phase + end * (kEvaluation.maximum_phase - phase)) /
        kEvaluation.maximum_phase;
}

int king_activity_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const std::uint8_t king = features.king_squares[own].index();
    if (king >= 64) {
        return 0;
    }

    const int file = king % 8;
    const int rank = king / 8;
    const int distance_from_center =
        std::min(std::abs(file - 3), std::abs(file - 4)) +
        std::min(std::abs(rank - 3), std::abs(rank - 4));
    const int centrality = std::max(0, 6 - distance_from_center);
    const int endgame_phase = kEvaluation.maximum_phase - features.game_phase;
    return centrality * kEvaluation.king_activity_weight * endgame_phase /
        kEvaluation.maximum_phase;
}

bool has_pawn_on_file(const PositionFeatures& features, int color, int file) noexcept {
    return file >= 0 && file < 8 &&
        (features.pawn_file_masks[static_cast<std::size_t>(color)] & (std::uint8_t{1} << file)) != 0;
}

bool has_any_pawn(const PositionFeatures& features) noexcept {
    return features.pawn_file_masks[0] != 0 || features.pawn_file_masks[1] != 0;
}

int pawn_structure_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int enemy = 1 - own;
    std::array<int, 8> own_file_counts{};
    std::array<int, 8> enemy_file_counts{};
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        if (piece.type != PieceType::pawn) {
            continue;
        }
        auto& counts = color_index(piece.color) == own ? own_file_counts : enemy_file_counts;
        ++counts[square % 8];
    }

    int score = 0;
    int islands = 0;
    bool previous_file = false;
    for (int file = 0; file < 8; ++file) {
        const bool occupied = own_file_counts[file] > 0;
        if (occupied && !previous_file) {
            ++islands;
        }
        previous_file = occupied;
        score -= std::max(0, own_file_counts[file] - 1) * kEvaluation.doubled_pawn_penalty;
        const bool left_file = file > 0 && own_file_counts[file - 1] > 0;
        const bool right_file = file < 7 && own_file_counts[file + 1] > 0;
        if (occupied && !left_file && !right_file) {
            score -= own_file_counts[file] * kEvaluation.isolated_pawn_penalty;
        }
    }
    score -= std::max(0, islands - 1) * kEvaluation.pawn_island_penalty;

    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece pawn = features.board[square];
        if (pawn.type != PieceType::pawn || color_index(pawn.color) != own) {
            continue;
        }

        const int file = square % 8;
        const int rank = square / 8;
        const int direction = color == Color::white ? 1 : -1;
        const int forward_rank = rank + direction;
        bool passed = true;
        for (int candidate_file = std::max(0, file - 1); candidate_file <= std::min(7, file + 1);
             ++candidate_file) {
            for (int candidate_rank = rank + direction; candidate_rank >= 0 && candidate_rank < 8;
                 candidate_rank += direction) {
                const Piece candidate = features.board[static_cast<std::size_t>(candidate_rank * 8 + candidate_file)];
                if (candidate.type == PieceType::pawn && color_index(candidate.color) == enemy) {
                    passed = false;
                }
            }
        }
        if (passed) {
            const int advancement = color == Color::white ? rank : 7 - rank;
            const int passed_bonus = kEvaluation.passed_pawn_base +
                advancement * kEvaluation.passed_pawn_advance_weight;
            score += passed_bonus * (kEvaluation.passed_pawn_phase_base - features.game_phase) /
                kEvaluation.maximum_phase;
            if ((features.attacked_squares[own] & (std::uint64_t{1} << square)) != 0) {
                score += kEvaluation.passed_pawn_protection_bonus;
            }
            if (forward_rank >= 0 && forward_rank < 8) {
                const Piece blocker = features.board[static_cast<std::size_t>(forward_rank * 8 + file)];
                if (!blocker.empty() && color_index(blocker.color) == enemy) {
                    score -= kEvaluation.direct_passed_pawn_blockade_penalty;
                }
            }
        }

        bool connected = false;
        for (int adjacent_file : {file - 1, file + 1}) {
            if (adjacent_file < 0 || adjacent_file >= 8) {
                continue;
            }
            for (int adjacent_rank = std::max(0, rank - 1); adjacent_rank <= std::min(7, rank + 1);
                 ++adjacent_rank) {
                const Piece adjacent = features.board[static_cast<std::size_t>(adjacent_rank * 8 + adjacent_file)];
                if (adjacent.type == PieceType::pawn && color_index(adjacent.color) == own) {
                    connected = true;
                }
            }
        }
        if (connected) {
            score += kEvaluation.connected_pawn_bonus;
        }

        if (forward_rank >= 0 && forward_rank < 8) {
            const std::uint64_t forward_bit = std::uint64_t{1} << (forward_rank * 8 + file);
            if ((features.attacked_squares[enemy] & forward_bit) != 0) {
                score -= kEvaluation.attacked_pawn_penalty;
            }

            bool has_advanced_support = false;
            for (const int adjacent_file : {file - 1, file + 1}) {
                if (adjacent_file < 0 || adjacent_file >= 8) {
                    continue;
                }
                for (int candidate_rank = forward_rank;
                     candidate_rank >= 0 && candidate_rank < 8;
                     candidate_rank += direction) {
                    const Piece candidate = features.board[
                        static_cast<std::size_t>(candidate_rank * 8 + adjacent_file)];
                    if (candidate.type == PieceType::pawn && color_index(candidate.color) == own) {
                        has_advanced_support = true;
                        break;
                    }
                }
                if (has_advanced_support) {
                    break;
                }
            }
            if (!has_advanced_support && (features.attacked_squares[enemy] & forward_bit) != 0) {
                score -= kEvaluation.backward_pawn_penalty;
            }
        }
    }
    return score;
}

int passed_pawn_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int direction = color == Color::white ? 1 : -1;
    const std::uint8_t king = features.king_squares[own].index();
    const int king_file = king < 64 ? king % 8 : 0;
    const int king_rank = king < 64 ? king / 8 : 0;
    const int endgame_phase = kEvaluation.maximum_phase - features.game_phase;
    int score = 0;

    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece pawn = features.board[square];
        if (pawn.type != PieceType::pawn || color_index(pawn.color) != own) {
            continue;
        }

        const int file = square % 8;
        const int rank = square / 8;
        bool passed = true;
        for (int candidate_file = std::max(0, file - 1);
             candidate_file <= std::min(7, file + 1); ++candidate_file) {
            for (int candidate_rank = rank + direction;
                 candidate_rank >= 0 && candidate_rank < 8;
                 candidate_rank += direction) {
                const Piece candidate = features.board[
                    static_cast<std::size_t>(candidate_rank * 8 + candidate_file)];
                if (candidate.type == PieceType::pawn && color_index(candidate.color) != own) {
                    passed = false;
                }
            }
        }
        if (!passed) {
            continue;
        }

        const int king_distance = std::max(std::abs(file - king_file), std::abs(rank - king_rank));
        if (king_distance <= 2) {
            score += kEvaluation.passed_pawn_king_support_bonus;
        }
        score += std::max(0, 4 - king_distance) *
            kEvaluation.passed_pawn_king_proximity_weight;

        const int advancement = color == Color::white ? rank : 7 - rank;
        score += advancement * kEvaluation.passed_pawn_promotion_weight * endgame_phase /
            kEvaluation.maximum_phase;
    }
    return score;
}

int activity_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int enemy = 1 - own;
    int score = 0;
    int bishops = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        if (piece.empty() || color_index(piece.color) != own) {
            continue;
        }
        if (piece.type == PieceType::bishop) {
            ++bishops;
            score += sliding_mobility(features, square, PieceType::bishop) * kEvaluation.bishop_mobility_weight;
        } else if (piece.type == PieceType::knight) {
            const int file = square % 8;
            const int rank = square / 8;
            if (file >= 2 && file <= 5 && rank >= 2 && rank <= 5 &&
                (features.attacked_squares[enemy] & (std::uint64_t{1} << square)) == 0) {
                score += kEvaluation.knight_outpost_bonus;
            }
        } else if (piece.type == PieceType::rook) {
            const int file = square % 8;
            if (!has_pawn_on_file(features, own, file)) {
                score += has_pawn_on_file(features, enemy, file) ?
                    kEvaluation.rook_semi_open_file_bonus : kEvaluation.rook_open_file_bonus;
            }
            if ((color == Color::white && square / 8 == 6) ||
                (color == Color::black && square / 8 == 1)) {
                score += kEvaluation.rook_seventh_rank_bonus;
            }
        } else if (piece.type == PieceType::queen) {
            score += sliding_mobility(features, square, PieceType::queen) * kEvaluation.queen_mobility_weight;
        }
    }
    if (bishops >= 2) {
        score += kEvaluation.bishop_pair_bonus;
    }
    return score;
}

int king_safety_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int enemy = 1 - own;
    const std::uint8_t king = features.king_squares[own].index();
    if (king >= 64) {
        return 0;
    }

    const int file = king % 8;
    const int rank = king / 8;
    const int shield_rank = rank + (color == Color::white ? 1 : -1);
    int score = 0;
    if (shield_rank >= 0 && shield_rank < 8) {
        for (int shield_file = std::max(0, file - 1); shield_file <= std::min(7, file + 1);
             ++shield_file) {
            const Piece shield = features.board[static_cast<std::size_t>(shield_rank * 8 + shield_file)];
            if (shield.type == PieceType::pawn && color_index(shield.color) == own) {
                score += kEvaluation.king_shield_bonus;
            }
        }
    }
    if (!has_pawn_on_file(features, own, file)) {
        score -= kEvaluation.king_open_file_penalty;
    }
    const std::uint64_t zone = king_zone(king);
    score -= std::popcount(features.attacked_squares[enemy] & zone) * kEvaluation.king_zone_attack_penalty;
    return score;
}

} // namespace

EvaluationBreakdown ClassicalEvaluator::breakdown(const GameState& state, Color perspective) const {
    const PositionFeatures features = state.position_features();
    EvaluationBreakdown score;
    const bool dead_material = insufficient_material(features);

    const int phase = features.game_phase;

    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        if (piece.empty()) {
            continue;
        }
        const int signed_value = piece_value(piece.type) * (piece.color == Color::white ? 1 : -1);
        score.material += signed_value;
        const std::uint8_t table_square = piece.color == Color::white ? square : mirrored_square(square);
        const int value = tapered_piece_square(piece.type, table_square, phase);
        score.piece_square += piece.color == Color::white ? value : -value;
    }

    score.mobility = (static_cast<int>(features.mobility[0]) -
                      static_cast<int>(features.mobility[1])) * kEvaluation.mobility_weight;
    score.pawn_structure = pawn_structure_for(features, Color::white) -
                           pawn_structure_for(features, Color::black);
    score.activity = activity_for(features, Color::white) - activity_for(features, Color::black);
    score.king_safety = king_safety_for(features, Color::white) -
                        king_safety_for(features, Color::black);
    score.king_safety = score.king_safety * (phase + kEvaluation.king_safety_phase_offset) /
        kEvaluation.king_safety_phase_divisor;
    score.king_activity = has_any_pawn(features) ?
        king_activity_for(features, Color::white) - king_activity_for(features, Color::black) : 0;
    score.passed_pawn = passed_pawn_for(features, Color::white) -
                        passed_pawn_for(features, Color::black);
    score.tempo = has_any_pawn(features) ?
        (features.side_to_move == Color::white ? kEvaluation.tempo_bonus :
         -kEvaluation.tempo_bonus) : 0;
    score.total = score.material + score.piece_square + score.mobility + score.pawn_structure +
                  score.activity + score.king_safety + score.king_activity + score.passed_pawn +
                  score.tempo;
    if (dead_material) {
        score.total = 0;
    }

    if (perspective == Color::black) {
        score.material = -score.material;
        score.piece_square = -score.piece_square;
        score.mobility = -score.mobility;
        score.pawn_structure = -score.pawn_structure;
        score.activity = -score.activity;
        score.king_safety = -score.king_safety;
        score.king_activity = -score.king_activity;
        score.passed_pawn = -score.passed_pawn;
        score.tempo = -score.tempo;
        score.total = -score.total;
    }
    return score;
}

int ClassicalEvaluator::evaluate(const GameState& state, Color perspective) const {
    return breakdown(state, perspective).total;
}

} // namespace koi
