#include "koi/evaluation_features.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "koi/detail/attack_tables.hpp"

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

// NNUE features are expressed from the side to move's perspective: squares are
// mirrored vertically and piece colours are swapped so the mover always looks
// like white. This is what lets one network distinguish positions that differ
// only in whose turn it is, and it is the standard half-king-style convention.
[[nodiscard]] constexpr std::size_t perspective_square(const std::size_t square,
                                                       const Color side) noexcept {
    return side == Color::white ? square : square ^ 56U;
}

[[nodiscard]] constexpr std::size_t perspective_color(const Color color,
                                                      const Color side) noexcept {
    return color == side ? kWhite : kBlack;
}

[[nodiscard]] NnueFeatureVectorV1 encode_piece_square(const EvaluationFeatures& features) noexcept {
    NnueFeatureVectorV1 encoded{};
    const Color mover = features.position.side_to_move;
    for (std::size_t square = 0; square < features.position.board.size(); ++square) {
        const Piece piece = features.position.board[square];
        if (piece.empty() || piece.type == PieceType::king) {
            continue;
        }
        const std::size_t color_offset = perspective_color(piece.color, mover) * 6U;
        const std::size_t type_offset = static_cast<std::size_t>(piece.type) - 1U;
        encoded[(color_offset + type_offset) * 64U + perspective_square(square, mover)] = 1;
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
    const Color mover = features.position.side_to_move;
    for (std::size_t square = 0; square < features.position.board.size(); ++square) {
        const Piece pawn = features.position.board[square];
        if (pawn.type != PieceType::pawn) {
            continue;
        }
        const std::size_t color = perspective_color(pawn.color, mover);
        const std::size_t file = square % 8U;
        result.counts[color][file] = static_cast<std::uint8_t>(
            std::min<unsigned>(255U, result.counts[color][file] + 1U));
    }

    for (std::size_t color = 0; color < 2; ++color) {
        const Color own_color = color == kWhite ? mover : opposite(mover);
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
                if (pawn.type != PieceType::pawn ||
                    perspective_color(pawn.color, mover) != color ||
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

// King-bucket index of a perspective own-king square: rank zones of three,
// three and two ranks, and four mirrored files.  Mirroring the file keeps the
// buckets symmetric for kings on the e-h files.
[[nodiscard]] constexpr std::size_t king_bucket(
    const std::size_t perspective_king_square) noexcept {
    const std::size_t file = perspective_king_square % 8U;
    const std::size_t rank = perspective_king_square / 8U;
    const std::size_t zone = rank <= 2U ? 0U : (rank <= 5U ? 1U : 2U);
    const std::size_t mirrored_file = file < 4U ? file + 4U : file;
    return zone * 4U + (mirrored_file - 4U);
}

// Collects the active halfka-king-bucket-v1 indices for one position under an
// explicit real-color perspective into a strictly increasing list capped at
// the sparse capacity.  When `dense` is non-null the same indices are also
// marked in the dense vector, so both encoders are guaranteed to agree.
[[nodiscard]] std::size_t halfka_king_bucket_indices(
    const EvaluationFeatures& features, const Color perspective, std::uint16_t* output,
    const std::size_t capacity, NnueFeatureVectorV4* dense = nullptr) noexcept {
    const std::uint8_t king_square =
        features.position.king_squares[color_index(perspective)].index();
    const std::size_t bucket = king_square < Square::kInvalid ?
        king_bucket(perspective_square(king_square, perspective)) : 0U;
    const std::size_t base = bucket * kNnuePieceSquareV1FeatureCount;
    std::size_t count = 0;
    for (std::size_t square = 0; square < features.position.board.size(); ++square) {
        const Piece piece = features.position.board[square];
        if (piece.empty()) {
            continue;
        }
        const std::size_t color = perspective_color(piece.color, perspective);
        const std::size_t type = static_cast<std::size_t>(piece.type);
        const std::size_t plane = color == kWhite ? type - 1U : type + 5U;
        const std::size_t index =
            base + plane * 64U + perspective_square(square, perspective);
        if (dense != nullptr) {
            (*dense)[index] = 1;
        }
        if (count < capacity) {
            output[count++] = static_cast<std::uint16_t>(index);
        }
    }
    std::sort(output, output + count);
    return count;
}

[[nodiscard]] NnueFeatureVectorV4 encode_halfka_king_bucket(
    const EvaluationFeatures& features) noexcept {
    NnueFeatureVectorV4 encoded{};
    std::array<std::uint16_t, kNnueSparseFeatureCapacityV4> scratch{};
    (void)halfka_king_bucket_indices(features, features.position.side_to_move,
                                     scratch.data(), scratch.size(), &encoded);
    return encoded;
}

constexpr std::size_t kThreatPawnOffset = 0;
constexpr std::size_t kThreatKnightOffset = 384;
constexpr std::size_t kThreatSliderOffset = 768;
constexpr std::size_t kThreatKingOffset = 1920;
constexpr std::size_t kThreatSliderStride = 384;

// Collects the active threat-pairs-v1 indices for one position under an
// explicit real-color perspective.  Every attack relation in the position is
// encoded (either colour attacking the other), expressed in the requested
// perspective's coordinates, so both perspectives see the same relations.  The
// result is sorted and deduplicated because two attackers of the same
// non-slider type can produce the same feature.
[[nodiscard]] std::size_t threat_pairs_indices(
    const EvaluationFeatures& features, const Color perspective,
    std::uint16_t* output, const std::size_t capacity,
    NnueFeatureVectorV5* dense = nullptr) noexcept {
    const auto& position = features.position;
    const std::size_t bucket =
        EvaluationFeatureExtractor::halfka_king_bucket_for(features, perspective);
    const std::size_t base =
        kNnueHalfkaKingBucketV1FeatureCount + bucket * kNnueThreatPairsBucketFeatureCount;

    std::uint64_t white_occupancy = 0;
    std::uint64_t black_occupancy = 0;
    for (std::size_t square = 0; square < position.board.size(); ++square) {
        const Piece piece = position.board[square];
        if (piece.empty()) {
            continue;
        }
        const std::uint64_t bit = std::uint64_t{1} << square;
        if (piece.color == Color::white) {
            white_occupancy |= bit;
        } else {
            black_occupancy |= bit;
        }
    }
    // Symmetric threats: the feature set records every attack relation in the
    // position (either colour attacking the other), encoded in the requested
    // perspective's coordinates.  Both perspectives therefore see the same
    // relation set, which keeps the dual-perspective pairing exact.
    if (white_occupancy == 0 || black_occupancy == 0) {
        return 0;
    }

    // Upper bound on emitted entries: both colours together can produce at most
    // 256 attacker/victim pairs (16 pieces per side).
    std::array<std::uint16_t, 256> scratch{};
    std::size_t scratch_count = 0;
    const std::uint64_t occupied = white_occupancy | black_occupancy;

    for (std::size_t square = 0; square < position.board.size(); ++square) {
        const Piece piece = position.board[square];
        if (piece.empty()) {
            continue;
        }
        const std::uint64_t targets =
            piece.color == Color::white ? black_occupancy : white_occupancy;
        std::uint64_t attacks = 0;
        switch (piece.type) {
        case PieceType::pawn:
            attacks = detail::pawn_attacks(static_cast<int>(square), piece.color == Color::white);
            break;
        case PieceType::knight:
            attacks = detail::knight_attacks(static_cast<int>(square));
            break;
        case PieceType::bishop:
            attacks = detail::bishop_attacks(static_cast<int>(square), occupied);
            break;
        case PieceType::rook:
            attacks = detail::rook_attacks(static_cast<int>(square), occupied);
            break;
        case PieceType::queen:
            attacks = detail::queen_attacks(static_cast<int>(square), occupied);
            break;
        case PieceType::king:
            attacks = detail::king_attacks(static_cast<int>(square));
            break;
        case PieceType::none:
            break;
        }

        std::uint64_t victims = attacks & targets;
        if (victims == 0) {
            continue;
        }
        const std::size_t attacker_square = perspective_square(square, perspective);
        while (victims != 0) {
            const std::size_t victim_square = std::countr_zero(victims);
            victims &= victims - 1;
            const Piece victim = position.board[victim_square];
            const std::size_t victim_type = static_cast<std::size_t>(victim.type) - 1U;
            const std::size_t victim_perspective = perspective_square(victim_square, perspective);
            std::size_t offset = 0;
            switch (piece.type) {
            case PieceType::pawn:
                offset = kThreatPawnOffset + victim_type * 64U + victim_perspective;
                break;
            case PieceType::knight:
                offset = kThreatKnightOffset + victim_type * 64U + victim_perspective;
                break;
            case PieceType::bishop:
                offset = kThreatSliderOffset + attacker_square * 6U + victim_type;
                break;
            case PieceType::rook:
                offset = kThreatSliderOffset + kThreatSliderStride +
                    attacker_square * 6U + victim_type;
                break;
            case PieceType::queen:
                offset = kThreatSliderOffset + 2U * kThreatSliderStride +
                    attacker_square * 6U + victim_type;
                break;
            case PieceType::king:
                offset = kThreatKingOffset + victim_type * 64U + victim_perspective;
                break;
            case PieceType::none:
                continue;
            }
            const std::size_t index = base + offset;
            if (dense != nullptr) {
                (*dense)[index] = 1;
            }
            if (scratch_count < scratch.size()) {
                scratch[scratch_count++] = static_cast<std::uint16_t>(index);
            }
        }
    }

    std::sort(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(scratch_count));
    const auto unique_end =
        std::unique(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(scratch_count));
    const std::size_t unique_count =
        static_cast<std::size_t>(std::distance(scratch.begin(), unique_end));
    const std::size_t count = std::min(unique_count, capacity);
    std::copy_n(scratch.begin(), count, output);
    return count;
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
        const Color king_color = color == kWhite ? features.position.side_to_move :
                                                   opposite(features.position.side_to_move);
        const std::uint8_t king_square = features.position.king_squares[
            color_index(king_color)].index();
        if (king_square < Square::kInvalid) {
            encoded[kNnuePieceSquareV1FeatureCount + color * 64U +
                    perspective_square(king_square, features.position.side_to_move)] = 1;
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

NnueSparseFeatures EvaluationFeatureExtractor::encode_sparse_v2(
    const EvaluationFeatures& features) noexcept {
    const NnueFeatureVectorV2 dense = encode_piece_square_king_pawn_v2(features);
    NnueSparseFeatures sparse;
    for (std::size_t index = 0; index < dense.size(); ++index) {
        if (dense[index] == 0) {
            continue;
        }
        if (sparse.count >= sparse.indices.size()) {
            break;
        }
        sparse.indices[sparse.count++] = static_cast<std::uint16_t>(index);
    }
    return sparse;
}

NnueFeatureVectorV4 EvaluationFeatureExtractor::encode_halfka_king_bucket_v1(
    const EvaluationFeatures& features) noexcept {
    return encode_halfka_king_bucket(features);
}

std::size_t EvaluationFeatureExtractor::halfka_king_bucket_for(
    const EvaluationFeatures& features, const Color perspective) noexcept {
    const std::uint8_t king_square =
        features.position.king_squares[color_index(perspective)].index();
    return king_square < Square::kInvalid ?
        king_bucket(perspective_square(king_square, perspective)) : 0U;
}

std::size_t EvaluationFeatureExtractor::halfka_king_bucket_for_square(
    const Square king_square, const Color perspective) noexcept {
    return king_square.index() < Square::kInvalid ?
        king_bucket(perspective_square(king_square.index(), perspective)) : 0U;
}

std::uint16_t EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
    const std::size_t bucket, const Color perspective, const Piece piece,
    const Square square) noexcept {
    const std::size_t color = perspective_color(piece.color, perspective);
    const std::size_t type = static_cast<std::size_t>(piece.type);
    const std::size_t plane = color == kWhite ? type - 1U : type + 5U;
    return static_cast<std::uint16_t>(
        bucket * kNnuePieceSquareV1FeatureCount + plane * 64U +
        perspective_square(square.index(), perspective));
}

NnueSparseFeaturesV4 EvaluationFeatureExtractor::encode_sparse_v4(
    const EvaluationFeatures& features) noexcept {
    return encode_sparse_v4(features, features.position.side_to_move);
}

NnueSparseFeaturesV4 EvaluationFeatureExtractor::encode_sparse_v4(
    const EvaluationFeatures& features, const Color perspective) noexcept {
    NnueSparseFeaturesV4 sparse;
    sparse.count = halfka_king_bucket_indices(
        features, perspective, sparse.indices.data(), sparse.indices.size());
    return sparse;
}

NnueSparseFeaturesThreatV1 EvaluationFeatureExtractor::encode_sparse_threat_v1(
    const EvaluationFeatures& features, const Color perspective) noexcept {
    NnueSparseFeaturesThreatV1 sparse;
    sparse.count = threat_pairs_indices(
        features, perspective, sparse.indices.data(), sparse.indices.size());
    return sparse;
}

NnueSparseFeaturesV5 EvaluationFeatureExtractor::encode_sparse_v5(
    const EvaluationFeatures& features, const Color perspective) noexcept {
    NnueSparseFeaturesV5 sparse;
    const NnueSparseFeaturesV4 group_a = encode_sparse_v4(features, perspective);
    const NnueSparseFeaturesThreatV1 group_b =
        encode_sparse_threat_v1(features, perspective);
    std::size_t count = 0;
    for (std::size_t i = 0; i < group_a.count; ++i) {
        sparse.indices[count++] = group_a.indices[i];
    }
    for (std::size_t i = 0; i < group_b.count; ++i) {
        sparse.indices[count++] = group_b.indices[i];
    }
    std::sort(sparse.indices.begin(),
              sparse.indices.begin() + static_cast<std::ptrdiff_t>(count));
    const auto unique_end =
        std::unique(sparse.indices.begin(),
                    sparse.indices.begin() + static_cast<std::ptrdiff_t>(count));
    sparse.count = std::min<std::size_t>(
        static_cast<std::size_t>(std::distance(sparse.indices.begin(), unique_end)),
        sparse.indices.size());
    return sparse;
}

NnueSparseFeaturesV5 EvaluationFeatureExtractor::encode_sparse_v5(
    const EvaluationFeatures& features) noexcept {
    return encode_sparse_v5(features, features.position.side_to_move);
}

NnueFeatureVectorV5 EvaluationFeatureExtractor::encode_halfka_threat_v5(
    const EvaluationFeatures& features, const Color perspective) noexcept {
    NnueFeatureVectorV5 encoded{};
    const NnueSparseFeaturesV4 group_a = encode_sparse_v4(features, perspective);
    for (std::size_t i = 0; i < group_a.count; ++i) {
        encoded[group_a.indices[i]] = 1;
    }
    std::array<std::uint16_t, kNnueSparseFeatureCapacityThreatV1> scratch{};
    (void)threat_pairs_indices(features, perspective, scratch.data(), scratch.size(),
                               &encoded);
    return encoded;
}

} // namespace koi
