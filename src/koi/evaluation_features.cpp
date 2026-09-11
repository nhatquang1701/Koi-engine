#include "koi/evaluation_features.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace koi {

namespace {

constexpr std::size_t kWhite = 0;
constexpr std::size_t kBlack = 1;
constexpr std::size_t kPawnPresenceFlag = 0;
constexpr std::size_t kDoubledPawnFlag = 1;
constexpr std::size_t kIsolatedPawnFlag = 2;
constexpr std::size_t kPassedPawnFlag = 3;

[[nodiscard]] constexpr std::size_t color_index(const Color color) noexcept {
    return color == Color::white ? kWhite : kBlack;
}

[[nodiscard]] NnueFeatureVectorV1 encode_piece_square(const EvaluationFeatures& features) noexcept {
    NnueFeatureVectorV1 encoded{};
    for (std::size_t square = 0; square < features.position.board.size(); ++square) {
        const Piece piece = features.position.board[square];
        if (piece.empty() || piece.type == PieceType::king) {
            continue;
        }
        const std::size_t color_offset = color_index(piece.color) * 6U;
        const std::size_t type_offset = static_cast<std::size_t>(piece.type) - 1U;
        encoded[(color_offset + type_offset) * 64U + square] = 1;
    }
    return encoded;
}

struct PawnFileState {
    std::array<std::array<std::uint8_t, 8>, 2> counts{};
    std::array<std::array<bool, 8>, 2> isolated{};
    std::array<std::array<bool, 8>, 2> passed{};
};

[[nodiscard]] PawnFileState pawn_file_state(const EvaluationFeatures& features) noexcept {
    PawnFileState result;
    for (std::size_t square = 0; square < features.position.board.size(); ++square) {
        const Piece pawn = features.position.board[square];
        if (pawn.type != PieceType::pawn) {
            continue;
        }
        const std::size_t color = color_index(pawn.color);
        const std::size_t file = square % 8U;
        result.counts[color][file] = static_cast<std::uint8_t>(
            std::min<unsigned>(255U, result.counts[color][file] + 1U));
    }

    for (std::size_t color = 0; color < 2; ++color) {
        const Color own_color = color == kWhite ? Color::white : Color::black;
        const Color enemy_color = opposite(own_color);
        for (std::size_t file = 0; file < 8; ++file) {
            const bool has_pawn = result.counts[color][file] != 0;
            if (!has_pawn) {
                continue;
            }
            const bool adjacent_own_pawn =
                (file > 0 && result.counts[color][file - 1] != 0) ||
                (file + 1 < 8 && result.counts[color][file + 1] != 0);

            bool has_isolated_pawn = false;
            bool has_passed_pawn = false;
            for (std::size_t square = 0; square < features.position.board.size(); ++square) {
                const Piece pawn = features.position.board[square];
                if (pawn.type != PieceType::pawn || pawn.color != own_color ||
                    square % 8U != file) {
                    continue;
                }
                if (!adjacent_own_pawn) {
                    has_isolated_pawn = true;
                }

                const int rank = static_cast<int>(square / 8U);
                bool enemy_ahead = false;
                for (std::size_t enemy_square = 0;
                     enemy_square < features.position.board.size(); ++enemy_square) {
                    const Piece enemy = features.position.board[enemy_square];
                    if (enemy.type != PieceType::pawn || enemy.color != enemy_color) {
                        continue;
                    }
                    const int enemy_file = static_cast<int>(enemy_square % 8U);
                    const int enemy_rank = static_cast<int>(enemy_square / 8U);
                    const bool ahead = own_color == Color::white ? enemy_rank > rank :
                                                                   enemy_rank < rank;
                    if (ahead && std::abs(enemy_file - static_cast<int>(file)) <= 1) {
                        enemy_ahead = true;
                        break;
                    }
                }
                if (!enemy_ahead) {
                    has_passed_pawn = true;
                }
            }
            result.isolated[color][file] = has_isolated_pawn;
            result.passed[color][file] = has_passed_pawn;
        }
    }
    return result;
}

} // namespace

EvaluationFeatures EvaluationFeatureExtractor::extract(const GameState& state) noexcept {
    EvaluationFeatures features;
    features.position = state.position_features();
    features.castling_rights = features.position.castling_rights;
    return features;
}

NnueFeatureVectorV1 EvaluationFeatureExtractor::encode_piece_square_v1(
    const EvaluationFeatures& features) noexcept {
    return encode_piece_square(features);
}

NnueFeatureVectorV2 EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(
    const EvaluationFeatures& features) noexcept {
    NnueFeatureVectorV2 encoded{};
    const NnueFeatureVectorV1 piece_square = encode_piece_square(features);
    std::copy(piece_square.begin(), piece_square.end(), encoded.begin());

    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint8_t king_square = features.position.king_squares[color].index();
        if (king_square < Square::kInvalid) {
            encoded[kNnuePieceSquareV1FeatureCount + color * 64U + king_square] = 1;
        }
    }

    const PawnFileState pawns = pawn_file_state(features);
    for (std::size_t color = 0; color < 2; ++color) {
        for (std::size_t file = 0; file < 8; ++file) {
            const std::size_t base = kNnuePieceSquareV1FeatureCount +
                kNnueKingContextFeatureCount + color * 32U + file * 4U;
            encoded[base + kPawnPresenceFlag] = pawns.counts[color][file] != 0 ? 1 : 0;
            encoded[base + kDoubledPawnFlag] = pawns.counts[color][file] >= 2 ? 1 : 0;
            encoded[base + kIsolatedPawnFlag] = pawns.isolated[color][file] ? 1 : 0;
            encoded[base + kPassedPawnFlag] = pawns.passed[color][file] ? 1 : 0;
        }
    }
    return encoded;
}

} // namespace koi
