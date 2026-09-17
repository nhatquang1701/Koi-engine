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

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests = {
        {"evaluation features v1 startpos", test_v1_startpos_encodes_pieces_only},
        {"evaluation features v2 startpos", test_v2_startpos_extends_v1_with_context},
        {"evaluation features perspective", test_black_to_move_mirrors_the_board},
        {"evaluation features sparse view", test_sparse_view_matches_the_dense_encoding},
        {"evaluation features pawn flags", test_pawn_file_flags_track_structure},
        {"evaluation features castling rights", test_extract_carries_castling_rights},
    };
    return koi::test::run_tests(tests, argc, argv);
}
