#include "koi/classical_evaluator.hpp"
#include "koi/evaluation_features.hpp"
#include "koi/piece_values.hpp"

#include "koi/detail/attack_tables.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>

namespace koi {
namespace {

constexpr const auto& kEvaluation = kClassicalEvaluationParameters;

constexpr int piece_value(PieceType type) noexcept {
    // Kings are never exchanged; the evaluator keeps them out of the material
    // term even though the shared table gives them a counting value.
    return type == PieceType::king ? 0 : piece_material_value(type);
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

// Occupancy masks derived from the cached mailbox board. Bitboard attack
// queries need these, and deriving them here keeps the evaluator independent
// of the native position's internal bitboards.
struct FeatureMasks {
    std::uint64_t occupied = 0;
    std::array<std::uint64_t, 2> colors{};
};

FeatureMasks feature_masks(const PositionFeatures& features) noexcept {
    FeatureMasks masks;
    for (std::size_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        if (piece.empty()) {
            continue;
        }
        const std::uint64_t bit = std::uint64_t{1} << square;
        masks.occupied |= bit;
        masks.colors[color_index(piece.color)] |= bit;
    }
    return masks;
}

int sliding_mobility(const PositionFeatures& features, std::uint8_t square,
                     PieceType type) noexcept {
    if (square >= 64) {
        return 0;
    }
    const Piece moving = features.board[square];
    if (moving.empty()) {
        return 0;
    }
    const FeatureMasks masks = feature_masks(features);
    const std::uint64_t attacks = type == PieceType::bishop ?
        detail::bishop_attacks(square, masks.occupied) :
        (type == PieceType::rook ? detail::rook_attacks(square, masks.occupied) :
                                   detail::queen_attacks(square, masks.occupied));
    return std::popcount(attacks & ~masks.colors[color_index(moving.color)]);
}

int tapered_piece_square(PieceType type, std::uint8_t square, int phase) noexcept {
    const int middle = middle_game_table(type)[square];
    const int end = end_game_table(type)[square];
    return (middle * phase + end * (kEvaluation.maximum_phase - phase)) /
        kEvaluation.maximum_phase;
}

int development_for(const EvaluationFeatures& extracted, Color color) noexcept {
    const PositionFeatures& features = extracted.position;
    const int phase = features.game_phase;
    const int own = color_index(color);
    const std::uint8_t king_side = color == Color::white ? kWhiteKingSideCastling :
        kBlackKingSideCastling;
    const std::uint8_t queen_side = color == Color::white ? kWhiteQueenSideCastling :
        kBlackQueenSideCastling;
    const int castling_rights =
        ((extracted.castling_rights & king_side) != 0 ? 1 : 0) +
        ((extracted.castling_rights & queen_side) != 0 ? 1 : 0);
    const int readiness = castling_rights * kEvaluation.castling_readiness_bonus;
    int opening_development =
        static_cast<int>(features.development[own]) * kEvaluation.development_bonus + readiness;

    // A queen sortie before the minor pieces are developed spends an opening
    // tempo and often becomes a target. Keep this deliberately narrow: it is
    // active only while the position still has opening material and only for
    // squares on the moving side's half of the board. Once three minor pieces
    // are developed, normal queen activity is left to mobility and tactical
    // search rather than being penalized here.
    if (phase >= 16 && features.fullmove_number <= 6 && features.development[own] < 3) {
        const std::uint8_t home_square = color == Color::white ? 3 : 59;
        int queen_development_penalty = 0;
        const bool queen_on_early_square = [&]() noexcept {
            for (std::uint8_t square = 0; square < 64; ++square) {
                const Piece piece = features.board[square];
                if (piece.type != PieceType::queen || piece.color != color) {
                    continue;
                }
                const int rank = square / 8;
                const bool on_own_half = color == Color::white ? rank <= 3 : rank >= 4;
                if (square == home_square || !on_own_half) {
                    return false;
                }
                const int file = square % 8;
                queen_development_penalty = file <= 1 || file >= 6 ?
                    kEvaluation.early_queen_development_penalty :
                    kEvaluation.central_queen_development_penalty;
                return true;
            }
            return false;
        }();
        if (queen_on_early_square) {
            opening_development -= queen_development_penalty;
        }
    }

    return opening_development * phase / kEvaluation.maximum_phase;
}

int center_control_for(const PositionFeatures& features, Color color) noexcept {
    const int phase = features.game_phase;
    return static_cast<int>(features.center_control[color_index(color)]) *
        kEvaluation.center_control_weight * (phase + kEvaluation.center_control_phase_offset) /
        (kEvaluation.maximum_phase + kEvaluation.center_control_phase_offset);
}

int knight_mobility(const PositionFeatures& features, std::uint8_t square) noexcept {
    if (square >= 64) {
        return 0;
    }
    const Piece moving = features.board[square];
    if (moving.empty()) {
        return 0;
    }
    const FeatureMasks masks = feature_masks(features);
    return std::popcount(detail::knight_attacks(square) & ~masks.colors[color_index(moving.color)]);
}

bool piece_attacks_square(const PositionFeatures& features, std::uint8_t source,
                          std::uint8_t target, std::uint64_t occupied) noexcept {
    if (source >= 64 || target >= 64 || source == target) {
        return false;
    }
    const Piece piece = features.board[source];
    if (piece.empty()) {
        return false;
    }

    const std::uint64_t target_bit = std::uint64_t{1} << target;
    switch (piece.type) {
    case PieceType::pawn:
        return (detail::pawn_attacks(source, piece.color == Color::white) & target_bit) != 0;
    case PieceType::knight:
        return (detail::knight_attacks(source) & target_bit) != 0;
    case PieceType::king:
        return (detail::king_attacks(source) & target_bit) != 0;
    case PieceType::bishop:
        return (detail::bishop_attacks(source, occupied) & target_bit) != 0;
    case PieceType::rook:
        return (detail::rook_attacks(source, occupied) & target_bit) != 0;
    case PieceType::queen:
        return (detail::queen_attacks(source, occupied) & target_bit) != 0;
    case PieceType::none:
        return false;
    }
    return false;
}

int king_ring_attack_units(const PositionFeatures& features, Color color) noexcept {
    // In sparse endgames the king is an active piece and static ring pressure
    // is too noisy; the tapered king-activity and passed-pawn terms are the
    // authoritative signals there.  Keep this middlegame safety feature out
    // of bare-queen/king tactical references as well as practical endgames.
    const auto piece_count = static_cast<int>(std::count_if(
        features.board.begin(), features.board.end(), [](const Piece piece) {
            return !piece.empty();
        }));
    if (features.game_phase < 8 || piece_count <= 8 || features.fullmove_number <= 10) {
        return 0;
    }

    const std::uint8_t castling_rights = color == Color::white ?
        kWhiteKingSideCastling | kWhiteQueenSideCastling :
        kBlackKingSideCastling | kBlackQueenSideCastling;
    if ((features.castling_rights & castling_rights) != 0) {
        // A king that can still castle is not committed to the current ring;
        // let the castling-readiness term and search decide whether to stay.
        return 0;
    }

    const std::uint8_t king = features.king_squares[color_index(color)].index();
    if (king >= 64) {
        return 0;
    }

    // `attacked_squares[color]` is the union of every attack mask for that
    // color, so whether a king square is attacked is a single bit test instead
    // of a full-board scan with sliding-piece ray walks.
    const auto king_is_attacked = [&features](Color victim) noexcept {
        const std::uint8_t victim_king = features.king_squares[color_index(victim)].index();
        if (victim_king >= 64) {
            return false;
        }
        const Color attacker = opposite(victim);
        return (features.attacked_squares[color_index(attacker)] &
                (std::uint64_t{1} << victim_king)) != 0;
    };

    // Checked positions are already handled by complete evasion, quiescence,
    // and mate-distance search. Avoid adding a second static king-ring signal
    // for either side while a forcing check is on the board.
    if (king_is_attacked(color) || king_is_attacked(opposite(color))) {
        return 0;
    }

    const int king_file = king % 8;
    const int king_rank = king / 8;
    const Color attacker = opposite(color);
    const FeatureMasks masks = feature_masks(features);

    int units = 0;
    for (std::uint8_t source = 0; source < 64; ++source) {
        const Piece piece = features.board[source];
        if (piece.empty() || piece.color != attacker) {
            continue;
        }

        bool attacks_ring = false;
        for (int file = king_file - 1; file <= king_file + 1 && !attacks_ring; ++file) {
            for (int rank = king_rank - 1; rank <= king_rank + 1; ++rank) {
                if (!inside(file, rank) || (file == king_file && rank == king_rank)) {
                    continue;
                }
                if (piece_attacks_square(features, source,
                                         static_cast<std::uint8_t>(rank * 8 + file),
                                         masks.occupied)) {
                    attacks_ring = true;
                    break;
                }
            }
        }
        if (!attacks_ring) {
            continue;
        }

        switch (piece.type) {
        case PieceType::pawn:
            units += 1;
            break;
        case PieceType::knight:
        case PieceType::bishop:
            units += 2;
            break;
        case PieceType::rook:
            units += 3;
            break;
        case PieceType::queen:
            units += 5;
            break;
        case PieceType::king:
            units += 1;
            break;
        case PieceType::none:
            break;
        }
    }
    return units;
}

int piece_mobility(const PositionFeatures& features, std::uint8_t square) noexcept {
    const Piece piece = features.board[square];
    if (piece.type == PieceType::knight) {
        return knight_mobility(features, square);
    }
    if (piece.type == PieceType::bishop || piece.type == PieceType::rook ||
        piece.type == PieceType::queen) {
        return sliding_mobility(features, square, piece.type);
    }
    return 1;
}

int pawn_break_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int enemy = 1 - own;
    const int direction = color == Color::white ? 1 : -1;
    int potential = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece pawn = features.board[square];
        if (pawn.type != PieceType::pawn || color_index(pawn.color) != own) {
            continue;
        }
        const int file = square % 8;
        const int rank = square / 8;
        const int forward_rank = rank + direction;
        if (!inside(file, forward_rank) ||
            !features.board[static_cast<std::size_t>(forward_rank * 8 + file)].empty()) {
            continue;
        }
        bool has_break = false;
        for (const int adjacent_file : {file - 1, file + 1}) {
            if (!inside(adjacent_file, forward_rank)) {
                continue;
            }
            const Piece target = features.board[
                static_cast<std::size_t>(forward_rank * 8 + adjacent_file)];
            if (target.type == PieceType::pawn && color_index(target.color) == enemy) {
                has_break = true;
            }
        }
        if (has_break) {
            ++potential;
        }
    }
    return potential * kEvaluation.pawn_break_bonus *
        (static_cast<int>(features.game_phase) + kEvaluation.center_control_phase_offset) /
        (kEvaluation.maximum_phase + kEvaluation.center_control_phase_offset);
}

int initiative_for(const PositionFeatures& features, Color color) noexcept {
    const int own = color_index(color);
    const int enemy = 1 - own;
    const int phase_taper = static_cast<int>(features.game_phase) +
        kEvaluation.center_control_phase_offset;
    int score = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece piece = features.board[square];
        if (piece.empty() || piece.type == PieceType::king) {
            continue;
        }
        const std::uint64_t square_bit = std::uint64_t{1} << square;
        if (color_index(piece.color) == enemy) {
            // An attack on a defended piece is not, by itself, a useful threat:
            // rewarding it inflated quiet positions where the opponent can
            // simply recapture the attacker.  Keep this term conservative and
            // reserve it for genuinely loose valuable pieces.
            if ((features.attacked_squares[own] & square_bit) != 0 &&
                (features.attacked_squares[enemy] & square_bit) == 0) {
                score += piece_value(piece.type) * kEvaluation.attacked_piece_pressure_weight / 100;
            }
            continue;
        }

        if ((features.attacked_squares[enemy] & square_bit) != 0 &&
            (features.attacked_squares[own] & square_bit) == 0) {
            score -= piece_value(piece.type) * kEvaluation.hanging_piece_penalty / 100;
        }
        if (piece.type == PieceType::knight && piece_mobility(features, square) == 0) {
            score -= kEvaluation.trapped_piece_penalty * phase_taper /
                (kEvaluation.maximum_phase + kEvaluation.center_control_phase_offset);
        }
    }
    return score;
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
    score -= static_cast<int>(features.king_zone_attacks[own]) * kEvaluation.king_zone_attack_penalty;
    score -= king_ring_attack_units(features, color) * kEvaluation.king_attacker_weight;
    return score;
}

} // namespace

EvaluationBreakdown ClassicalEvaluator::breakdown(const GameState& state, Color perspective) const {
    const EvaluationFeatures extracted = EvaluationFeatureExtractor::extract(state);
    const PositionFeatures& features = extracted.position;
    EvaluationBreakdown score;
    // One authoritative dead-position test: the native position already
    // recognizes insufficient material (including same-complex bishop endings)
    // and the known locked-pawn-wall dead position, with its en-passant
    // exception. Routing through it here keeps the evaluator's zero from
    // drifting away from the rules layer.
    const bool dead_position = state.is_dead_position();

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
    score.development = development_for(extracted, Color::white) -
                        development_for(extracted, Color::black);
    score.center_control = center_control_for(features, Color::white) -
                           center_control_for(features, Color::black) +
                           pawn_break_for(features, Color::white) -
                           pawn_break_for(features, Color::black);
    score.initiative = initiative_for(features, Color::white) -
                       initiative_for(features, Color::black);
    score.king_safety = king_safety_for(features, Color::white) -
                        king_safety_for(features, Color::black);
    score.king_safety = score.king_safety * (phase + kEvaluation.king_safety_phase_offset) /
        kEvaluation.king_safety_phase_divisor;
    score.king_activity = phase <= 2 ?
        king_activity_for(features, Color::white) - king_activity_for(features, Color::black) : 0;
    score.passed_pawn = passed_pawn_for(features, Color::white) -
                        passed_pawn_for(features, Color::black);
    score.tempo = phase <= 2 ?
        (features.side_to_move == Color::white ? kEvaluation.tempo_bonus :
         -kEvaluation.tempo_bonus) : 0;
    score.total = score.material + score.piece_square + score.mobility + score.pawn_structure +
                  score.activity + score.development + score.center_control + score.initiative +
                  score.king_safety + score.king_activity + score.passed_pawn + score.tempo;
    if (dead_position) {
        score.total = 0;
    }

    if (perspective == Color::black) {
        score.material = -score.material;
        score.piece_square = -score.piece_square;
        score.mobility = -score.mobility;
        score.pawn_structure = -score.pawn_structure;
        score.activity = -score.activity;
        score.development = -score.development;
        score.center_control = -score.center_control;
        score.initiative = -score.initiative;
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
