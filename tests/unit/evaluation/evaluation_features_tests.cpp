// Focused unit coverage for the evaluation feature extractor: the dense v1 and
// v2 NNUE encoders, the sparse v2 view used by inference, perspective handling
// and the pawn-file structure flags.  The NNUE container tests keep covering
// loader/inference; this suite pins feature encoding itself.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "koi/evaluation_features.hpp"
#include "koi/game_state.hpp"
#include "koi_test_support.hpp"

namespace {

using koi::EvaluationFeatureExtractor;
using koi::EvaluationFeatures;
using koi::GameState;
using koi::NnueFeatureVectorV1;
using koi::NnueFeatureVectorV2;
using koi::NnueSparseFeatures;
using koi::test::require;

constexpr std::string_view kStartpos = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

[[nodiscard]] GameState state_from(std::string_view fen) {
    return koi::test::require_value(GameState::from_fen(fen), "fixture FEN must parse");
}

[[nodiscard]] EvaluationFeatures features_from(std::string_view fen) {
    return EvaluationFeatureExtractor::extract(state_from(fen));
}

[[nodiscard]] std::size_t count_active(const NnueFeatureVectorV1& encoded) {
    std::size_t active = 0;
    for (const std::int8_t value : encoded) {
        require(value == 0 || value == 1, "v1 inputs must be binary");
        active += value != 0 ? 1U : 0U;
    }
    return active;
}

[[nodiscard]] std::size_t count_active(const NnueFeatureVectorV2& encoded) {
    std::size_t active = 0;
    for (const std::int8_t value : encoded) {
        require(value == 0 || value == 1, "v2 inputs must be binary");
        active += value != 0 ? 1U : 0U;
    }
    return active;
}

void test_v1_startpos_encodes_pieces_only() {
    const NnueFeatureVectorV1 encoded =
        EvaluationFeatureExtractor::encode_piece_square_v1(features_from(kStartpos));

    require(count_active(encoded) == 30, "startpos has thirty non-king pieces");
    for (std::size_t index = 5 * 64; index < 6 * 64; ++index) {
        require(encoded[index] == 0, "the mover's king plane must stay empty");
    }
    for (std::size_t index = 11 * 64; index < 12 * 64; ++index) {
        require(encoded[index] == 0, "the opponent's king plane must stay empty");
    }
    require(encoded[8] == 1, "the mover's a2 pawn must set its own plane");
    require(encoded[6 * 64 + 48] == 1, "the opponent's a7 pawn must set the mirrored plane");
}

void test_v2_startpos_extends_v1_with_context() {
    const EvaluationFeatures features = features_from(kStartpos);
    const NnueFeatureVectorV1 v1 = EvaluationFeatureExtractor::encode_piece_square_v1(features);
    const NnueFeatureVectorV2 v2 =
        EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(features);

    for (std::size_t index = 0; index < v1.size(); ++index) {
        require(v2[index] == v1[index], "the v2 piece-square prefix must match v1");
    }
    require(count_active(v2) == 48,
            "startpos must add two king contexts and sixteen pawn-presence flags");

    // White king e1 (square 4) is the mover context; black king e8 (square 60)
    // is the opponent context.
    require(v2[768 + 4] == 1, "the mover's king context must be set");
    require(v2[768 + 64 + 60] == 1, "the opponent's king context must be set");

    // a-file pawns: own presence is set, and no doubled/isolated/passed flags
    // apply to the starting position.
    require(v2[768 + 128] == 1, "the mover's a-file pawn presence must be set");
    require(v2[768 + 128 + 1] == 0, "startpos must not report doubled pawns");
    require(v2[768 + 128 + 2] == 0, "startpos must not report isolated pawns");
    require(v2[768 + 128 + 3] == 0, "startpos must not report passed pawns");
    require(v2[768 + 128 + 32] == 1, "the opponent's a-file pawn presence must be set");
}

void test_black_to_move_mirrors_the_board() {
    // A single white pawn on e4 with black to move: the opponent pawn must land
    // on the mirrored e5 square of the opponent plane.
    const NnueFeatureVectorV1 encoded = EvaluationFeatureExtractor::encode_piece_square_v1(
        features_from("4k3/8/8/8/4P3/8/8/4K3 b - - 0 1"));

    require(count_active(encoded) == 1, "only the white pawn is a non-king piece");
    require(encoded[6 * 64 + 36] == 1,
            "the white e4 pawn must appear mirrored as an opponent pawn on e5");
}

void test_sparse_view_matches_the_dense_encoding() {
    const std::vector<std::string_view> fens = {
        kStartpos,
        "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    };
    for (const std::string_view fen : fens) {
        const EvaluationFeatures features = features_from(fen);
        const NnueFeatureVectorV2 dense =
            EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(features);
        const NnueSparseFeatures sparse = EvaluationFeatureExtractor::encode_sparse_v2(features);

        require(sparse.count == count_active(dense),
                "the sparse count must match the dense active inputs");
        for (std::size_t index = 0; index < sparse.count; ++index) {
            require(dense[sparse.indices[index]] == 1,
                    "every sparse index must name an active dense input");
            if (index > 0) {
                require(sparse.indices[index - 1] < sparse.indices[index],
                        "sparse indices must be strictly increasing");
            }
        }
    }
}

void test_pawn_file_flags_track_structure() {
    // Doubled a-pawns on a4/a3 with no other pawns: the a-file must flag
    // presence, doubled, isolated and passed; the b-file must stay clear.
    const NnueFeatureVectorV2 encoded = EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(
        features_from("4k3/8/8/8/P7/P7/8/4K3 w - - 0 1"));

    constexpr std::size_t own_base = 768 + 128;
    require(encoded[own_base + 0] == 1, "the doubled a-file must report presence");
    require(encoded[own_base + 1] == 1, "the doubled a-file must report the doubled flag");
    require(encoded[own_base + 2] == 1, "the isolated a-file must report the isolated flag");
    require(encoded[own_base + 3] == 1, "the a-file pawns must report a passed pawn");
    for (std::size_t flag = 0; flag < 4; ++flag) {
        require(encoded[own_base + 4 + flag] == 0, "the empty b-file must stay clear");
    }

    // Black has no pawns, so every opponent pawn flag must be zero.
    for (std::size_t index = own_base + 32; index < 768 + 128 + 64; ++index) {
        require(encoded[index] == 0, "a pawnless side must report no pawn flags");
    }
}

void test_extract_carries_castling_rights() {
    const EvaluationFeatures start = features_from(kStartpos);
    require(start.castling_rights != 0, "startpos must expose castling rights");

    const EvaluationFeatures quiet = features_from("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(quiet.castling_rights == 0, "a position without rights must report none");
}

[[nodiscard]] std::size_t count_active(const koi::NnueFeatureVectorV4& encoded) {
    std::size_t active = 0;
    for (const std::int8_t value : encoded) {
        require(value == 0 || value == 1, "v4 inputs must be binary");
        active += value != 0 ? 1U : 0U;
    }
    return active;
}

[[nodiscard]] std::vector<std::size_t> sparse_indices(const koi::NnueSparseFeaturesV4& sparse) {
    std::vector<std::size_t> indices;
    indices.reserve(sparse.count);
    for (std::size_t index = 0; index < sparse.count; ++index) {
        if (index > 0) {
            require(sparse.indices[index - 1] < sparse.indices[index],
                    "v4 sparse indices must be strictly increasing");
        }
        indices.push_back(sparse.indices[index]);
    }
    return indices;
}

// The halfka-king-bucket-v1 golden vectors pin both the king-bucket base and
// the 12-plane piece mapping.  The same literal list is asserted from the
// Python encoder in the Phase 5 parity test.
const std::vector<std::size_t> kStartposV4Indices = {
    8, 9, 10, 11, 12, 13, 14, 15, 65, 70, 130, 133, 192, 199, 259, 324,
    432, 433, 434, 435, 436, 437, 438, 439, 505, 510, 570, 573, 632, 639, 699, 764,
};
const std::vector<std::size_t> kMidgameV4Indices = {
    8, 9, 10, 11, 13, 14, 15, 28, 70, 82, 130, 133, 192, 199, 259, 324,
    420, 432, 433, 434, 435, 437, 438, 439, 493, 505, 546, 570, 632, 639, 699, 764,
};

void test_v4_startpos_golden_vector_ignores_the_turn() {
    const EvaluationFeatures white = features_from(kStartpos);
    const auto dense = EvaluationFeatureExtractor::encode_halfka_king_bucket_v1(white);
    const auto sparse = EvaluationFeatureExtractor::encode_sparse_v4(white);
    require(dense.size() == 9216, "halfka-king-bucket-v1 must contain 9216 inputs");
    require(count_active(dense) == 32, "startpos has thirty-two active v4 inputs");
    require(sparse_indices(sparse) == kStartposV4Indices,
            "the startpos golden sparse vector must match the pinned index list");
    for (const std::size_t index : kStartposV4Indices) {
        require(dense[index] == 1, "every golden index must be active in the dense vector");
    }

    // The black-to-move startpos mirrors squares and swaps colours, so the
    // encoded vector is identical.
    const auto black = EvaluationFeatureExtractor::encode_sparse_v4(
        features_from("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1"));
    require(sparse_indices(black) == kStartposV4Indices,
            "the startpos vector must not depend on whose turn it is");
}

void test_v4_midgame_golden_vector() {
    const auto sparse = EvaluationFeatureExtractor::encode_sparse_v4(
        features_from("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4"));
    require(sparse_indices(sparse) == kMidgameV4Indices,
            "the midgame golden sparse vector must match the pinned index list");
}

void test_v4_king_bucket_tracks_the_own_king() {
    // e4 (bucket 4) from white's view and e5 (mirrored to e4) from black's view
    // must select the same bucket base for both kings.
    const std::vector<std::size_t> king_on_e4 = {3420, 3836};
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_v4(
                features_from("4k3/8/8/8/4K3/8/8/8 w - - 0 1"))) == king_on_e4,
            "a white king on e4 must use rank zone one and the mirrored file");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_v4(
                features_from("8/8/8/4k3/8/8/8/4K3 b - - 0 1"))) == king_on_e4,
            "a black king on e5 must mirror into the same bucket from black's view");

    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_v4(
                features_from("4k3/8/8/8/8/8/8/K7 w - - 0 1"))) ==
                std::vector<std::size_t>({320, 764}),
            "a king on a1 must use the low rank zone and the mirrored a-file");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_v4(
                features_from("4k3/8/8/K7/8/8/8/8 w - - 0 1"))) ==
                std::vector<std::size_t>({3424, 3836}),
            "a king on a5 must use the middle rank zone of the same mirrored file");
}

void test_v4_sparse_view_matches_the_dense_encoding() {
    const std::vector<std::string_view> fens = {
        kStartpos,
        "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "4k3/8/8/8/8/8/8/4K3 b - - 0 1",
    };
    for (const std::string_view fen : fens) {
        const EvaluationFeatures features = features_from(fen);
        const auto dense = EvaluationFeatureExtractor::encode_halfka_king_bucket_v1(features);
        const auto sparse = EvaluationFeatureExtractor::encode_sparse_v4(features);
        require(sparse.count == count_active(dense),
                "the v4 sparse count must match the dense active inputs");
        for (std::size_t index = 0; index < sparse.count; ++index) {
            require(dense[sparse.indices[index]] == 1,
                    "every v4 sparse index must name an active dense input");
            if (index > 0) {
                require(sparse.indices[index - 1] < sparse.indices[index],
                        "v4 sparse indices must be strictly increasing");
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests = {
        {"evaluation features v1 startpos", test_v1_startpos_encodes_pieces_only},
        {"evaluation features v2 startpos", test_v2_startpos_extends_v1_with_context},
        {"evaluation features perspective", test_black_to_move_mirrors_the_board},
        {"evaluation features sparse view", test_sparse_view_matches_the_dense_encoding},
        {"evaluation features pawn flags", test_pawn_file_flags_track_structure},
        {"evaluation features castling rights", test_extract_carries_castling_rights},
        {"evaluation features v4 startpos", test_v4_startpos_golden_vector_ignores_the_turn},
        {"evaluation features v4 midgame", test_v4_midgame_golden_vector},
        {"evaluation features v4 king buckets", test_v4_king_bucket_tracks_the_own_king},
        {"evaluation features v4 sparse view", test_v4_sparse_view_matches_the_dense_encoding},
    };
    return koi::test::run_tests(tests, argc, argv);
}
