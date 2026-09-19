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

using koi::Color;
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

[[nodiscard]] std::size_t count_active(const koi::NnueFeatureVectorV5& encoded) {
    std::size_t active = 0;
    for (const std::int8_t value : encoded) {
        require(value == 0 || value == 1, "v5 inputs must be binary");
        active += value != 0 ? 1U : 0U;
    }
    return active;
}

[[nodiscard]] std::vector<std::size_t> sparse_indices(
    const koi::NnueSparseFeaturesThreatV1& sparse) {
    std::vector<std::size_t> indices;
    indices.reserve(sparse.count);
    for (std::size_t index = 0; index < sparse.count; ++index) {
        if (index > 0) {
            require(sparse.indices[index - 1] < sparse.indices[index],
                    "threat sparse indices must be strictly increasing");
        }
        indices.push_back(sparse.indices[index]);
    }
    return indices;
}

[[nodiscard]] std::vector<std::size_t> sparse_indices(const koi::NnueSparseFeaturesV5& sparse) {
    std::vector<std::size_t> indices;
    indices.reserve(sparse.count);
    for (std::size_t index = 0; index < sparse.count; ++index) {
        if (index > 0) {
            require(sparse.indices[index - 1] < sparse.indices[index],
                    "v5 sparse indices must be strictly increasing");
        }
        indices.push_back(sparse.indices[index]);
    }
    return indices;
}

// The Italian midgame owns two attack relations: the knight on f3 attacks the
// e5 pawn and the bishop on c4 attacks the f7 pawn.  Both land in bucket 0,
// with the knight on the victim-type/victim-square layout (384 + 0 * 64 + 36)
// and the bishop on the slider layout (768 + 0 * 384 + 26 * 6 + 0).
const std::vector<std::size_t> kMidgameWhiteThreats = {9636, 10140};
// The symmetric view from black's perspective mirrors those same relations.
const std::vector<std::size_t> kMidgameBlackThreats = {9628, 10188};

// The two-knights variation adds black's own attacks, so with symmetric threat
// pairs both perspectives encode all four relations.
const std::vector<std::size_t> kMidgameSymmetricThreats = {9628, 9636, 10140, 10188};

// The same midgame with white to move after 2.Nc3 Nf6 and 3.Bc5 in the
// two-knights variation: the halfka group is the midgame golden list of the v4
// tests, and the four symmetric threat inputs are appended.
const std::vector<std::size_t> kMidgameV5Indices = {
    8, 9, 10, 13, 14, 15, 19, 28, 82, 85, 130, 154, 192, 199, 259, 324,
    420, 432, 433, 434, 435, 437, 438, 439, 490, 493, 546, 570, 632, 639, 699, 764,
    9628, 9636, 10140, 10188,
};

void test_threat_v1_startpos_has_no_attacks() {
    const EvaluationFeatures features = features_from(kStartpos);
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                features, Color::white)).empty(),
            "the start position has no attacks on enemy pieces");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                features, Color::black)).empty(),
            "the start position has no attacks on enemy pieces for either side");

    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_v5(features)) ==
                kStartposV4Indices,
            "with an empty threat group the combined v5 list must equal the halfka list");
}

void test_threat_v1_golden_midgame_vectors() {
    const EvaluationFeatures black_to_move = features_from(
        "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                black_to_move, Color::black)) == kMidgameBlackThreats,
            "the symmetric black view must mirror the white attack relations");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                black_to_move, Color::white)) == kMidgameWhiteThreats,
            "the white knight and bishop attacks must match the golden threat list");

    const EvaluationFeatures white_to_move = features_from(
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                white_to_move, Color::white)) == kMidgameSymmetricThreats,
            "both sides' attacks must match the symmetric golden list");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                white_to_move, Color::black)) == kMidgameSymmetricThreats,
            "the mirrored black view must reuse the symmetric golden list");
}

void test_threat_v1_deduplicates_shared_attackers() {
    // Both white pawns (c5 and e5) attack the black d6 pawn, which must
    // collapse into one victim input instead of being counted twice; the
    // symmetric view also encodes the black pawn's attacks on both white pawns.
    const EvaluationFeatures features = features_from("4k3/8/3p4/2P1P3/8/8/8/4K3 w - - 0 1");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                features, Color::white)) == std::vector<std::size_t>({9250, 9252, 9259}),
            "two pawns attacking one victim must collapse into one threat input");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                features, Color::black)) == std::vector<std::size_t>({9235, 9242, 9244}),
            "the black pawn must pin both of its attacked white pawns");
}

void test_threat_v1_covers_every_attack_family() {
    // Knight, bishop and rook families: the knight hits the d4 pawn
    // (384 + 27) and the rook hits the b8 rook (768 + 384 + 6 + 3); black
    // answers with the bishop on the f3 knight (768 + 18 * 6 + 1) and the
    // rook on the b1 rook (768 + 384 + 6 + 3).
    const EvaluationFeatures families =
        features_from("1r2k3/8/2b5/8/3p4/5N2/8/1R2K3 w - - 0 1");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                families, Color::white)) == std::vector<std::size_t>({9627, 10237, 10377, 10713}),
            "the knight and rook attacks must match the family golden list");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                families, Color::black)) == std::vector<std::size_t>({9635, 10093, 10377, 10713}),
            "the black bishop and rook attacks must match the family golden list");

    // King family: the white king attacks the black knight on d2
    // (1920 + 1 * 64 + 11); the black knight answers on the b1 rook
    // (384 + 3 * 64 + 57).
    const EvaluationFeatures king_attacks =
        features_from("1r2k3/8/2b5/8/3p4/8/3n4/1R2K3 w - - 0 1");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                king_attacks, Color::white)) == std::vector<std::size_t>({9793, 10377, 10713, 11211}),
            "the king attack must use the king family offsets");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                king_attacks, Color::black)) == std::vector<std::size_t>({9849, 10377, 10713, 11251}),
            "the knight answer must use the knight family offsets");

    // Queen family: the queen slider index is two, so Qd1 on Qd8 is
    // 768 + 2 * 384 + 3 * 6 + 4.
    const EvaluationFeatures queens = features_from("3qk3/8/8/8/8/8/8/3QK3 w - - 0 1");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                queens, Color::white)) == std::vector<std::size_t>({10774, 11110}),
            "the queen attack must use slider index two");
    require(sparse_indices(EvaluationFeatureExtractor::encode_sparse_threat_v1(
                queens, Color::black)) == std::vector<std::size_t>({10774, 11110}),
            "the mirrored queen attack must reuse the same index");
}

void test_v5_sparse_merges_halfka_and_threats() {
    const EvaluationFeatures features = features_from(
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1");
    const auto sparse = EvaluationFeatureExtractor::encode_sparse_v5(features, Color::white);
    require(sparse_indices(sparse) == kMidgameV5Indices,
            "the combined v5 list must be the sorted merge of both groups");

    const auto dense = EvaluationFeatureExtractor::encode_halfka_threat_v5(features, Color::white);
    require(dense.size() == 36864, "the combined v5 vector must contain 36864 inputs");
    require(count_active(dense) == kMidgameV5Indices.size(),
            "the dense v5 vector must mark exactly the sparse inputs");
    for (const std::size_t index : kMidgameV5Indices) {
        require(dense[index] == 1, "every golden v5 index must be active in the dense vector");
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
        {"evaluation features threat startpos", test_threat_v1_startpos_has_no_attacks},
        {"evaluation features threat golden", test_threat_v1_golden_midgame_vectors},
        {"evaluation features threat dedupe", test_threat_v1_deduplicates_shared_attackers},
        {"evaluation features threat families", test_threat_v1_covers_every_attack_family},
        {"evaluation features v5 merge", test_v5_sparse_merges_halfka_and_threats},
    };
    return koi::test::run_tests(tests, argc, argv);
}
