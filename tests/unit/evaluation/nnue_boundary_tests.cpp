#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/evaluation_features.hpp"
#include "koi/nnue.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

std::optional<std::filesystem::path> external_container_path;

koi::GameState require_state(std::string_view fen) {
    return koi::test::require_value(koi::GameState::from_fen(fen),
                                    "test FEN must construct a game state");
}

void test_container_round_trip_and_validation() {
    const koi::NnueNetwork network = koi::NnueNetwork::synthetic();
    const auto encoded = koi::NnueLoader::serialize(network);
    require(encoded.has_value(), "synthetic NNUE network must serialize");

    const auto decoded = koi::NnueLoader::load(*encoded);
    require(decoded.has_value(), "serialized NNUE network must load");
    require(decoded->manifest.layer_sizes[0] == koi::kKoiNnueFeatureCount &&
                decoded->manifest.layer_sizes == koi::NnueLayerSizes{768, 128, 32, 1} &&
                decoded->manifest.feature_set == "piece-square-v1" &&
                decoded->manifest.quantization == "int16/int8" &&
                decoded->manifest.network_sha256 == std::array<std::uint8_t, 32>{
                    0xe8, 0x11, 0xb6, 0x99, 0x4a, 0x49, 0xe9, 0xd6,
                    0xc0, 0x08, 0x90, 0x1f, 0x71, 0xa4, 0x7f, 0x86,
                    0x9f, 0x58, 0xb0, 0xc5, 0xfe, 0x19, 0x51, 0x6b,
                    0x89, 0x09, 0x75, 0x30, 0x03, 0x0b, 0x77, 0x60},
            "NNUE manifest dimensions, feature set, and quantization must survive a round trip");

    std::vector<std::uint8_t> corrupt = *encoded;
    corrupt[0] ^= 0xFFU;
    const auto malformed = koi::NnueLoader::load(corrupt);
    require(!malformed.has_value() && malformed.error().code == koi::NnueErrorCode::bad_magic,
            "NNUE loader must reject malformed magic");

    std::vector<std::uint8_t> checksum_corrupt = *encoded;
    checksum_corrupt.back() ^= 0x01U;
    const auto checksum_failure = koi::NnueLoader::load(checksum_corrupt);
    require(!checksum_failure.has_value() &&
                checksum_failure.error().code == koi::NnueErrorCode::checksum_mismatch,
            "NNUE loader must reject a payload checksum mismatch");
}

void test_container_rejects_nonstandard_layer_shape() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic();
    network.manifest.layer_sizes[1] = 64;
    const auto encoded = koi::NnueLoader::serialize(network);
    require(!encoded.has_value() && encoded.error().code == koi::NnueErrorCode::invalid_dimensions,
            "NNUE serialization must reject a nonstandard Koi layer shape");
}

void test_malformed_or_absent_network_uses_classical_fallback() {
    const koi::GameState state = require_state("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    const koi::ClassicalEvaluator classical;
    const koi::test::TempDirectory scratch;

    const koi::EvaluatorSelection absent = koi::make_evaluator(std::nullopt);
    require(!absent.nnue_enabled && absent.evaluator &&
                absent.evaluator->evaluate(state, koi::Color::white) ==
                    classical.evaluate(state, koi::Color::white),
            "absent NNUE must select the classical evaluator");

    const std::filesystem::path malformed_path = scratch.file("koi-task4-malformed.nnue");
    {
        std::ofstream output(malformed_path, std::ios::binary | std::ios::trunc);
        output << "not a Koi NNUE";
    }
    const koi::EvaluatorSelection malformed = koi::make_evaluator(malformed_path);
    std::filesystem::remove(malformed_path);
    require(!malformed.nnue_enabled && malformed.nnue_error.has_value() && malformed.evaluator &&
                malformed.evaluator->evaluate(state, koi::Color::white) ==
                    classical.evaluate(state, koi::Color::white),
            "malformed NNUE must preserve the classical fallback");

    const std::filesystem::path valid_path = scratch.file("koi-v2-opt-in.nnue");
    const auto encoded = koi::NnueLoader::serialize(koi::NnueNetwork::synthetic_v2());
    require(encoded.has_value(), "valid NNUE opt-in fixture must serialize");
    {
        std::ofstream output(valid_path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(encoded->data()),
                     static_cast<std::streamsize>(encoded->size()));
    }
    const koi::EvaluatorSelection valid = koi::make_evaluator(valid_path);
    std::filesystem::remove(valid_path);
    require(valid.nnue_enabled && valid.evaluator && !valid.nnue_error.has_value() &&
                valid.evaluator->evaluate(state, koi::Color::white) ==
                    koi::NnueWorker(std::make_shared<const koi::NnueNetwork>(
                        koi::NnueNetwork::synthetic_v2())).evaluate(
                        koi::EvaluationFeatureExtractor::extract(state), koi::Color::white),
            "a valid NNUE path must opt in without changing the no-path default");
}

void test_workers_keep_immutable_weights_and_private_accumulators() {
    const auto network = std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic());
    const koi::NnueEvaluator evaluator(network);
    koi::NnueWorker first = evaluator.make_worker();
    koi::NnueWorker second = evaluator.make_worker();
    const koi::EvaluationFeatures opening =
        koi::EvaluationFeatureExtractor::extract(koi::GameState::startpos());

    require(first.evaluate(opening, koi::Color::white) ==
                second.evaluate(opening, koi::Color::white),
            "independent NNUE workers must be deterministic");
    const auto first_accumulator = first.accumulator().values;
    const auto endgame = koi::EvaluationFeatureExtractor::extract(
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"));
    (void)second.evaluate(endgame, koi::Color::white);
    require(first.accumulator().values == first_accumulator,
            "one worker's accumulator must not be mutated by another worker");
}

void test_v2_feature_vector_has_stable_king_and_pawn_context() {
    const koi::GameState state = require_state(
        "4k3/7p/8/8/8/P7/P7/4K3 w - - 0 1");
    const koi::EvaluationFeatures features =
        koi::EvaluationFeatureExtractor::extract(state);
    const auto encoded =
        koi::EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(features);

    require(encoded.size() == 960, "v2 NNUE encoding must contain 960 inputs");
    require(encoded[8] == 1 && encoded[16] == 1,
            "v2 must preserve the white pawn-square planes");
    require(encoded[768 + 4] == 1 && encoded[768 + 64 + 60] == 1,
            "v2 must encode white and black king squares");

    const std::size_t white_a_file = 896;
    require(encoded[white_a_file] == 1 && encoded[white_a_file + 1] == 1 &&
                encoded[white_a_file + 2] == 1 && encoded[white_a_file + 3] == 1,
            "white a-file must be present, doubled, isolated, and passed");
    const std::size_t black_h_file = 896 + 32 + 7 * 4;
    require(encoded[black_h_file] == 1 && encoded[black_h_file + 1] == 0 &&
                encoded[black_h_file + 2] == 1 && encoded[black_h_file + 3] == 1,
            "black h-file must expose all deterministic pawn flags");
    require(std::count(encoded.begin(), encoded.end(), std::int8_t{1}) == 12,
            "v2 golden fixture must have exactly twelve active inputs");
}

void test_v2_container_round_trip_is_deterministic() {
    const koi::NnueNetwork network = koi::NnueNetwork::synthetic_v2();
    const auto encoded = koi::NnueLoader::serialize(network);
    require(encoded.has_value(), "synthetic v2 NNUE network must serialize");
    const auto repeated = koi::NnueLoader::serialize(network);
    require(repeated.has_value() && *encoded == *repeated,
            "v2 NNUE serialization must be byte deterministic");

    const auto decoded = koi::NnueLoader::load(*encoded);
    require(decoded.has_value(), "serialized v2 NNUE network must load");
    require(decoded->manifest.feature_set == koi::kKoiNnuePieceSquareKingPawnV2FeatureSet &&
                decoded->manifest.layer_sizes == koi::kKoiNnuePieceSquareKingPawnV2LayerSizes &&
                decoded->feature_weights.size() == 960U * 256U &&
                decoded->manifest.payload_size > 0 &&
                decoded->manifest.network_sha256 == std::array<std::uint8_t, 32>{
                    0x31, 0xc8, 0xe2, 0x2b, 0xba, 0xcd, 0x22, 0x0c,
                    0x51, 0x0a, 0x8e, 0x22, 0xb5, 0x14, 0xef, 0xe9,
                    0x17, 0x5a, 0x90, 0x12, 0xd9, 0xab, 0xff, 0x84,
                    0xde, 0x84, 0x08, 0x1f, 0xc7, 0xc4, 0x09, 0x33} &&
                std::any_of(decoded->manifest.network_sha256.begin(),
                            decoded->manifest.network_sha256.end(),
                            [](std::uint8_t byte) { return byte != 0; }),
            "v2 manifest and checksum must survive a deterministic round trip");
}

void test_external_v2_container() {
    if (!external_container_path.has_value()) {
        koi::test::skip("no external container path provided");
    }
    const auto decoded = koi::NnueLoader::load_file(*external_container_path);
    require(decoded.has_value(), "external NNUE exporter output must load in Koi");
    require(decoded->manifest.feature_set == koi::kKoiNnuePieceSquareKingPawnV2FeatureSet &&
                decoded->manifest.layer_sizes == koi::kKoiNnuePieceSquareKingPawnV2LayerSizes,
            "external NNUE exporter output must use the Koi v2 manifest");
}

void test_v2_manifest_validation_rejects_mismatches() {
    koi::NnueNetwork unknown = koi::NnueNetwork::synthetic_v2();
    unknown.manifest.feature_set = "unknown-feature-set";
    const auto unknown_result = koi::NnueLoader::serialize(unknown);
    require(!unknown_result.has_value() &&
                unknown_result.error().code == koi::NnueErrorCode::unsupported_feature_set,
            "unknown NNUE feature sets must be rejected");

    koi::NnueNetwork wrong_shape = koi::NnueNetwork::synthetic_v2();
    wrong_shape.manifest.layer_sizes[1] = 128;
    const auto wrong_shape_result = koi::NnueLoader::serialize(wrong_shape);
    require(!wrong_shape_result.has_value() &&
                wrong_shape_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v2 networks with the legacy hidden width must be rejected");

    koi::NnueNetwork wrong_quantization = koi::NnueNetwork::synthetic_v2();
    wrong_quantization.manifest.quantization = "float32";
    const auto wrong_quantization_result = koi::NnueLoader::serialize(wrong_quantization);
    require(!wrong_quantization_result.has_value() &&
                wrong_quantization_result.error().code == koi::NnueErrorCode::unsupported_quantization,
            "unsupported NNUE quantization must be rejected");

    const auto encoded = koi::NnueLoader::serialize(koi::NnueNetwork::synthetic_v2());
    require(encoded.has_value(), "v2 validation fixture must serialize");
    std::vector<std::uint8_t> wrong_header = *encoded;
    wrong_header[12] = 0xBF;
    wrong_header[13] = 0x03;
    wrong_header[14] = 0;
    wrong_header[15] = 0;
    const auto wrong_header_result = koi::NnueLoader::load(wrong_header);
    require(!wrong_header_result.has_value() &&
                wrong_header_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "serialized v2 networks with a mismatched input count must be rejected");

    std::vector<std::uint8_t> truncated = *encoded;
    truncated.pop_back();
    const auto truncated_result = koi::NnueLoader::load(truncated);
    require(!truncated_result.has_value() &&
                truncated_result.error().code == koi::NnueErrorCode::invalid_file_size,
            "truncated NNUE payloads must be rejected before inference");
}

void test_v2_golden_vector_and_inference_paths() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v2();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    std::fill(network.output_weights.begin(), network.output_weights.end(), 0);
    network.output_bias = 5;

    constexpr std::size_t hidden = 256;
    network.feature_weights[8 * hidden] = 3;
    network.feature_weights[(768 + 4) * hidden] = 4;
    network.feature_weights[896 * hidden + 1] = 200;
    network.bottleneck_weights[0] = 2;
    network.bottleneck_weights[32] = 1;
    network.output_weights[0] = 2;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker worker(weights);
    const koi::EvaluationFeatures features = koi::EvaluationFeatureExtractor::extract(
        require_state("4k3/7p/8/8/8/P7/P7/4K3 w - - 0 1"));
    const int scalar = worker.evaluate(features, koi::Color::white,
                                       koi::NnueInferencePath::scalar);
    require(scalar == 259 && worker.accumulator().values[0] == 7 &&
                worker.accumulator().values[1] == 127 &&
                worker.accumulator().bottleneck_values[0] == 127,
            "v2 golden vector must apply clipped ReLU and output scaling exactly: score=" +
                std::to_string(scalar) + " hidden0=" +
                std::to_string(worker.accumulator().values[0]) + " hidden1=" +
                std::to_string(worker.accumulator().values[1]) + " bottleneck0=" +
                std::to_string(worker.accumulator().bottleneck_values[0]));
    const auto scalar_accumulator = worker.accumulator();
    const int avx2 = worker.evaluate(features, koi::Color::white,
                                     koi::NnueInferencePath::avx2_compatible);
    require(avx2 == scalar && worker.accumulator().values == scalar_accumulator.values &&
                worker.accumulator().bottleneck_values == scalar_accumulator.bottleneck_values,
            "scalar and AVX2-compatible NNUE paths must be deterministic and equal");
    require(worker.evaluate(features, koi::Color::black,
                            koi::NnueInferencePath::scalar) == -scalar,
            "NNUE perspective sign must be deterministic");
}

void test_avx2_compatible_path_preserves_wide_accumulation() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v2();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    std::fill(network.output_weights.begin(), network.output_weights.end(), 0);
    network.hidden_bias[0] = std::numeric_limits<std::int32_t>::max();
    network.feature_weights[8U * 256U] = 1;
    network.bottleneck_weights[0] = 1;
    network.output_weights[0] = 1;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    const koi::EvaluationFeatures features = koi::EvaluationFeatureExtractor::extract(
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"));
    koi::NnueWorker scalar_worker(weights);
    koi::NnueWorker avx2_worker(weights);
    const int scalar = scalar_worker.evaluate(features, koi::Color::white,
                                              koi::NnueInferencePath::scalar);
    const int avx2 = avx2_worker.evaluate(features, koi::Color::white,
                                          koi::NnueInferencePath::avx2_compatible);
    require(scalar == 127 && avx2 == scalar &&
                avx2_worker.accumulator().values[0] == scalar_worker.accumulator().values[0],
            "AVX2-compatible NNUE inference must preserve wide accumulation before clipping");
}

void test_invalid_network_does_not_allocate_worker_accumulators() {
    koi::NnueNetwork invalid = koi::NnueNetwork::synthetic_v2();
    invalid.manifest.layer_sizes[1] = 128;
    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(invalid));
    koi::NnueWorker worker(weights);
    require(worker.accumulator().values.empty() && worker.accumulator().bottleneck_values.empty(),
            "invalid NNUE manifests must not size worker accumulators before validation");
}

void test_v3_shift_explicit_inference_and_container() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v2();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    std::fill(network.output_weights.begin(), network.output_weights.end(), 0);
    network.output_bias = 0;
    network.manifest.version = koi::kKoiNnuePerspectiveV3FormatVersion;
    network.hidden_shift = 7;
    network.bottleneck_shift = 7;
    network.output_shift = 5;
    network.feature_weights[8U * 256U] = 128;
    network.bottleneck_weights[0] = 127;
    network.output_weights[0] = 25;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker worker(weights);
    const koi::EvaluationFeatures features = koi::EvaluationFeatureExtractor::extract(
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"));
    const int scalar = worker.evaluate(features, koi::Color::white,
                                       koi::NnueInferencePath::scalar);
    require(scalar == 98 && worker.accumulator().values[0] == 127 &&
                worker.accumulator().bottleneck_values[0] == 126,
            "v3 shift-explicit inference must apply arithmetic shifts exactly: score=" +
                std::to_string(scalar) + " hidden0=" +
                std::to_string(worker.accumulator().values[0]) + " bottleneck0=" +
                std::to_string(worker.accumulator().bottleneck_values[0]));
    const auto scalar_accumulator = worker.accumulator();
    const int avx2 = worker.evaluate(features, koi::Color::white,
                                     koi::NnueInferencePath::avx2_compatible);
    require(avx2 == scalar && worker.accumulator().values == scalar_accumulator.values &&
                worker.accumulator().bottleneck_values == scalar_accumulator.bottleneck_values,
            "v3 scalar and AVX2-compatible paths must agree");
    require(worker.evaluate(features, koi::Color::black,
                            koi::NnueInferencePath::scalar) == -scalar,
            "v3 perspective sign must match the v2 contract");

    const auto v2_reference = koi::NnueLoader::serialize(koi::NnueNetwork::synthetic_v2());
    require(v2_reference.has_value(), "v2 reference network must serialize");
    const auto encoded = koi::NnueLoader::serialize(*weights);
    require(encoded.has_value(), "v3 network must serialize");
    require(encoded->size() == v2_reference->size() + 4,
            "v3 containers must add exactly the four shift bytes");
    const auto decoded = koi::NnueLoader::load(*encoded);
    require(decoded.has_value(), "v3 container must load");
    require(decoded->manifest.version == koi::kKoiNnuePerspectiveV3FormatVersion &&
                decoded->hidden_shift == 7 && decoded->bottleneck_shift == 7 &&
                decoded->output_shift == 5 &&
                decoded->manifest.layer_sizes == weights->manifest.layer_sizes,
            "v3 shifts must survive the container round trip");
}

std::size_t reference_v4_bucket(const koi::EvaluationFeatures& features) {
    std::size_t pieces = 0;
    for (const koi::Piece& piece : features.position.board) {
        if (!piece.empty()) {
            ++pieces;
        }
    }
    return std::min<std::size_t>(7U, (32U - std::min<std::size_t>(32U, pieces)) / 4U);
}

int reference_v4_score(const koi::NnueNetwork& network,
                       const koi::NnueSparseFeaturesV4& sparse,
                       std::size_t bucket) {
    const std::size_t hidden = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden / 2U;
    std::vector<int> activations(hidden, 0);
    for (std::size_t hidden_index = 0; hidden_index < hidden; ++hidden_index) {
        std::int64_t value = network.hidden_bias[hidden_index];
        for (std::size_t index = 0; index < sparse.count; ++index) {
            value += network.feature_weights[
                static_cast<std::size_t>(sparse.indices[index]) * hidden + hidden_index];
        }
        value = std::clamp<std::int64_t>(value, std::numeric_limits<std::int32_t>::min(),
                                         std::numeric_limits<std::int32_t>::max());
        activations[hidden_index] = std::clamp<int>(static_cast<int>(value), 0,
                                                    koi::kKoiNnueClippedReluMaximum);
    }
    std::int64_t output = network.bottleneck_bias[bucket];
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
        output += static_cast<std::int64_t>(
                      network.bottleneck_weights[bucket * pair_count + pair]) *
            activations[pair] * activations[pair + pair_count];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return static_cast<int>(std::clamp<std::int64_t>(
        output, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

void test_v4_golden_pair_product_score() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    const koi::EvaluationFeatures features =
        koi::EvaluationFeatureExtractor::extract(koi::GameState::startpos());
    const koi::NnueSparseFeaturesV4 sparse =
        koi::EvaluationFeatureExtractor::encode_sparse_v4(features);
    require(sparse.count == 32, "the v4 golden fixture must use the thirty-two startpos inputs");

    constexpr std::size_t kHidden = 32;
    for (std::size_t index = 0; index < 10; ++index) {
        network.feature_weights[static_cast<std::size_t>(sparse.indices[index]) * kHidden] = 10;
    }
    for (std::size_t index = 10; index < 17; ++index) {
        network.feature_weights[
            static_cast<std::size_t>(sparse.indices[index]) * kHidden + kHidden / 2U] = 20;
    }
    for (std::size_t bucket = 0; bucket < network.bottleneck_bias.size(); ++bucket) {
        network.bottleneck_weights[bucket * (kHidden / 2U)] = 127;
    }

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker worker(weights);
    const int scalar = worker.evaluate(features, koi::Color::white,
                                       koi::NnueInferencePath::scalar);
    require(scalar == 49 && worker.accumulator().values[0] == 100 &&
                worker.accumulator().values[16] == 127 &&
                worker.accumulator().bottleneck_values[0] == 12700,
            "v4 golden pair-product inference must apply clipping and shifts exactly: score=" +
                std::to_string(scalar) + " hidden0=" +
                std::to_string(worker.accumulator().values[0]) + " hidden16=" +
                std::to_string(worker.accumulator().values[16]) + " pair0=" +
                std::to_string(worker.accumulator().bottleneck_values[0]));
    const int avx2 = worker.evaluate(features, koi::Color::white,
                                     koi::NnueInferencePath::avx2_compatible);
    require(avx2 == scalar, "v4 AVX2 pair products must reproduce the golden scalar score");
}

void test_v4_scalar_matches_independent_reference() {
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    koi::NnueWorker worker(weights);
    const std::vector<koi::GameState> states{
        koi::GameState::startpos(),
        require_state("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4"),
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"),
        require_state("4k3/8/8/8/8/8/8/4K3 w - - 0 1"),
    };
    for (const koi::GameState& state : states) {
        const koi::EvaluationFeatures features =
            koi::EvaluationFeatureExtractor::extract(state);
        const koi::NnueSparseFeaturesV4 sparse =
            koi::EvaluationFeatureExtractor::encode_sparse_v4(features);
        const int expected = reference_v4_score(*weights, sparse, reference_v4_bucket(features));
        const koi::Color mover = features.position.side_to_move;
        const int scalar = worker.evaluate(features, mover, koi::NnueInferencePath::scalar);
        require(scalar == expected,
                "v4 scalar inference must match the independent reference: " +
                    std::to_string(scalar) + " != " + std::to_string(expected));
        require(worker.evaluate(features, koi::opposite(mover),
                                koi::NnueInferencePath::scalar) == -scalar,
                "v4 perspective sign must mirror the caller's perspective");
    }
}

void test_v4_inference_paths_agree_on_random_weights() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] =
            static_cast<std::int16_t>(static_cast<int>(index * 37U % 2001U) - 1000);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] =
            static_cast<std::int32_t>(static_cast<int>(index * 97U % 251U) - 125);
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] =
            static_cast<std::int8_t>(static_cast<int>(index * 29U % 255U) - 127);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] =
            static_cast<std::int32_t>(static_cast<int>(index * 13U) - 50);
    }
    network.output_shift = 12;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker scalar_worker(weights);
    koi::NnueWorker avx2_worker(weights);
    const std::vector<koi::GameState> states{
        koi::GameState::startpos(),
        require_state("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4"),
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"),
    };
    for (const koi::GameState& state : states) {
        const koi::EvaluationFeatures features =
            koi::EvaluationFeatureExtractor::extract(state);
        const koi::NnueSparseFeaturesV4 sparse =
            koi::EvaluationFeatureExtractor::encode_sparse_v4(features);
        const int expected = reference_v4_score(*weights, sparse, reference_v4_bucket(features));
        const koi::Color mover = features.position.side_to_move;
        const int scalar = scalar_worker.evaluate(features, mover,
                                                  koi::NnueInferencePath::scalar);
        const int avx2 = avx2_worker.evaluate(features, mover,
                                              koi::NnueInferencePath::avx2_compatible);
        require(scalar == expected && avx2 == expected &&
                    avx2_worker.accumulator().values == scalar_worker.accumulator().values &&
                    avx2_worker.accumulator().bottleneck_values ==
                        scalar_worker.accumulator().bottleneck_values,
                "v4 scalar and AVX2 paths must agree on wide random weights: " +
                    std::to_string(scalar) + "/" + std::to_string(avx2) + " != " +
                    std::to_string(expected));
    }
}

void test_v4_piece_count_bucket_selects_the_head() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    constexpr std::size_t kHidden = 32;
    network.hidden_bias[0] = 100;
    network.hidden_bias[kHidden / 2U] = 100;
    network.bottleneck_weights[0] = 127;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker worker(weights);
    const koi::EvaluationFeatures startpos =
        koi::EvaluationFeatureExtractor::extract(koi::GameState::startpos());
    require(reference_v4_bucket(startpos) == 0, "a full board must select the first v4 head");
    const int full_board = worker.evaluate(startpos, koi::Color::white,
                                           koi::NnueInferencePath::scalar);
    require(full_board == (127 * 100 * 100) >> 15,
            "the first v4 head must produce the pair-product score");

    const koi::EvaluationFeatures sparse_board = koi::EvaluationFeatureExtractor::extract(
        require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1"));
    require(reference_v4_bucket(sparse_board) == 7,
            "a three-piece board must select the last v4 head");
    require(worker.evaluate(sparse_board, koi::Color::white,
                            koi::NnueInferencePath::scalar) == 0,
            "an empty piece-count head must score zero regardless of the hidden layer");
}

void test_v4_wide_accumulation_preserves_clipping() {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    std::fill(network.feature_weights.begin(), network.feature_weights.end(), 0);
    std::fill(network.hidden_bias.begin(), network.hidden_bias.end(), 0);
    std::fill(network.bottleneck_weights.begin(), network.bottleneck_weights.end(), 0);
    std::fill(network.bottleneck_bias.begin(), network.bottleneck_bias.end(), 0);
    constexpr std::size_t kHidden = 32;
    network.hidden_bias[0] = std::numeric_limits<std::int32_t>::max();
    network.hidden_bias[1] = std::numeric_limits<std::int32_t>::min();
    network.hidden_bias[kHidden / 2U] = 20;
    network.bottleneck_weights[0] = 127;

    const koi::EvaluationFeatures features =
        koi::EvaluationFeatureExtractor::extract(koi::GameState::startpos());
    const koi::NnueSparseFeaturesV4 sparse =
        koi::EvaluationFeatureExtractor::encode_sparse_v4(features);
    const std::size_t first = sparse.indices[0];
    network.feature_weights[first * kHidden] = 32767;
    network.feature_weights[first * kHidden + 1] = -5;

    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(network));
    koi::NnueWorker scalar_worker(weights);
    koi::NnueWorker avx2_worker(weights);
    const int scalar = scalar_worker.evaluate(features, koi::Color::white,
                                              koi::NnueInferencePath::scalar);
    const int avx2 = avx2_worker.evaluate(features, koi::Color::white,
                                          koi::NnueInferencePath::avx2_compatible);
    require(scalar == avx2 && scalar == 9 && scalar_worker.accumulator().values[0] == 127 &&
                scalar_worker.accumulator().values[1] == 0 &&
                scalar_worker.accumulator().values[kHidden / 2U] == 20,
            "v4 inference must clamp wide hidden sums before the pair products");
}

void test_v4_container_round_trip_and_validation() {
    const koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    const auto encoded = koi::NnueLoader::serialize(network);
    require(encoded.has_value(), "synthetic v4 NNUE network must serialize");
    const auto repeated = koi::NnueLoader::serialize(network);
    require(repeated.has_value() && *encoded == *repeated,
            "v4 NNUE serialization must be byte deterministic");

    constexpr std::size_t kInputUnits = 9216;
    constexpr std::size_t kHiddenUnits = 32;
    constexpr std::size_t kOutputBuckets = 8;
    constexpr std::size_t kPayloadSize = kInputUnits * kHiddenUnits * 2 +
        kHiddenUnits * 4 + kOutputBuckets * (kHiddenUnits / 2) + kOutputBuckets * 4;
    const std::size_t strings = koi::kKoiNnueQuantization.size() +
        koi::kKoiNnueHalfkaKingBucketV1FeatureSet.size();
    require(encoded->size() == 76 + strings + kPayloadSize,
            "v4 container size must match the manifest, strings and payload formula");

    const auto decoded = koi::NnueLoader::load(*encoded);
    require(decoded.has_value(), "serialized v4 NNUE network must load");
    require(decoded->manifest.version == koi::kKoiNnueHalfkaKingBucketV1FormatVersion &&
                decoded->manifest.layer_sizes == koi::NnueLayerSizes{9216, 32, 8, 0} &&
                decoded->manifest.feature_set == "halfka-king-bucket-v1" &&
                decoded->manifest.quantization == "int16/int8" &&
                decoded->hidden_shift == 7 && decoded->output_shift == 15 &&
                decoded->bottleneck_shift == 0 &&
                decoded->feature_weights.size() == kInputUnits * kHiddenUnits &&
                decoded->bottleneck_weights.size() == kOutputBuckets * (kHiddenUnits / 2) &&
                decoded->bottleneck_bias.size() == kOutputBuckets &&
                decoded->output_weights.empty(),
            "v4 manifest, shifts and per-bucket arrays must survive a round trip");
    require(decoded->manifest.network_sha256 == std::array<std::uint8_t, 32>{
                0x7d, 0x60, 0x17, 0x21, 0xc6, 0xaf, 0xb8, 0xcf,
                0xaf, 0x31, 0xb7, 0xb6, 0x8b, 0xd8, 0xa0, 0x57,
                0x78, 0xdb, 0x38, 0xfa, 0x33, 0x4e, 0x0a, 0x58,
                0x34, 0x35, 0xba, 0x5a, 0x66, 0x83, 0x00, 0xe8},
            "v4 payload checksum must be stable");
}

void test_v4_manifest_validation_rejects_mismatches() {
    koi::NnueNetwork legacy_features = koi::NnueNetwork::synthetic_v4();
    legacy_features.manifest.feature_set = "piece-square-king-pawn-v2";
    const auto legacy_features_result = koi::NnueLoader::serialize(legacy_features);
    require(!legacy_features_result.has_value() &&
                legacy_features_result.error().code ==
                    koi::NnueErrorCode::unsupported_feature_set,
            "v4 networks must reject the legacy feature sets");

    koi::NnueNetwork unknown = koi::NnueNetwork::synthetic_v4();
    unknown.manifest.feature_set = "unknown-feature-set";
    const auto unknown_result = koi::NnueLoader::serialize(unknown);
    require(!unknown_result.has_value() &&
                unknown_result.error().code == koi::NnueErrorCode::unsupported_feature_set,
            "v4 networks must reject unknown feature sets");

    koi::NnueNetwork odd_hidden = koi::NnueNetwork::synthetic_v4();
    odd_hidden.manifest.layer_sizes[1] = 31;
    const auto odd_hidden_result = koi::NnueLoader::serialize(odd_hidden);
    require(!odd_hidden_result.has_value() &&
                odd_hidden_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v4 networks must reject odd hidden widths");

    koi::NnueNetwork narrow_hidden = koi::NnueNetwork::synthetic_v4();
    narrow_hidden.manifest.layer_sizes[1] = 16;
    const auto narrow_hidden_result = koi::NnueLoader::serialize(narrow_hidden);
    require(!narrow_hidden_result.has_value() &&
                narrow_hidden_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v4 networks must reject hidden widths below the named minimum");

    koi::NnueNetwork wide_hidden = koi::NnueNetwork::synthetic_v4();
    wide_hidden.manifest.layer_sizes[1] = 8194;
    const auto wide_hidden_result = koi::NnueLoader::serialize(wide_hidden);
    require(!wide_hidden_result.has_value() &&
                wide_hidden_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v4 networks must reject hidden widths above the named maximum");

    koi::NnueNetwork wrong_buckets = koi::NnueNetwork::synthetic_v4();
    wrong_buckets.manifest.layer_sizes[2] = 4;
    const auto wrong_buckets_result = koi::NnueLoader::serialize(wrong_buckets);
    require(!wrong_buckets_result.has_value() &&
                wrong_buckets_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v4 networks must use exactly eight output buckets");

    koi::NnueNetwork reserved_word = koi::NnueNetwork::synthetic_v4();
    reserved_word.manifest.layer_sizes[3] = 1;
    const auto reserved_word_result = koi::NnueLoader::serialize(reserved_word);
    require(!reserved_word_result.has_value() &&
                reserved_word_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "v4 networks must keep the reserved layer word zero");

    koi::NnueNetwork bad_shift = koi::NnueNetwork::synthetic_v4();
    bad_shift.hidden_shift = 21;
    const auto bad_shift_result = koi::NnueLoader::serialize(bad_shift);
    require(!bad_shift_result.has_value() &&
                bad_shift_result.error().code == koi::NnueErrorCode::malformed_manifest,
            "v4 serialization must reject shifts above the named limit");

    koi::NnueNetwork bad_bottleneck = koi::NnueNetwork::synthetic_v4();
    bad_bottleneck.bottleneck_shift = 1;
    const auto bad_bottleneck_result = koi::NnueLoader::serialize(bad_bottleneck);
    require(!bad_bottleneck_result.has_value() &&
                bad_bottleneck_result.error().code == koi::NnueErrorCode::malformed_manifest,
            "v4 serialization must reject a nonzero bottleneck shift");
}

void test_v4_container_rejects_corruption_and_legacy_versions() {
    const auto encoded = koi::NnueLoader::serialize(koi::NnueNetwork::synthetic_v4());
    require(encoded.has_value(), "v4 corruption fixture must serialize");

    std::vector<std::uint8_t> reserved_bytes = *encoded;
    reserved_bytes[30] = 1;
    const auto reserved_bytes_result = koi::NnueLoader::load(reserved_bytes);
    require(!reserved_bytes_result.has_value() &&
                reserved_bytes_result.error().code == koi::NnueErrorCode::malformed_manifest,
            "nonzero v4 reserved scale bytes must be rejected");

    std::vector<std::uint8_t> reserved_layer = *encoded;
    reserved_layer[24] = 1;
    const auto reserved_layer_result = koi::NnueLoader::load(reserved_layer);
    require(!reserved_layer_result.has_value() &&
                reserved_layer_result.error().code == koi::NnueErrorCode::invalid_dimensions,
            "a nonzero v4 reserved layer word must be rejected");

    std::vector<std::uint8_t> corrupt_shift = *encoded;
    corrupt_shift[28] = 21;
    const auto corrupt_shift_result = koi::NnueLoader::load(corrupt_shift);
    require(!corrupt_shift_result.has_value() &&
                corrupt_shift_result.error().code == koi::NnueErrorCode::malformed_manifest,
            "v4 shifts above the named limit must be rejected before inference");

    std::vector<std::uint8_t> truncated = *encoded;
    truncated.pop_back();
    const auto truncated_result = koi::NnueLoader::load(truncated);
    require(!truncated_result.has_value() &&
                truncated_result.error().code == koi::NnueErrorCode::invalid_file_size,
            "truncated v4 payloads must be rejected");

    const std::vector<std::uint8_t> empty;
    const auto empty_result = koi::NnueLoader::load(empty);
    require(!empty_result.has_value() &&
                empty_result.error().code == koi::NnueErrorCode::empty_container,
            "an empty v4 container must be rejected");

    const std::vector<std::uint8_t> tiny(4, 0);
    const auto tiny_result = koi::NnueLoader::load(tiny);
    require(!tiny_result.has_value() &&
                tiny_result.error().code == koi::NnueErrorCode::invalid_file_size,
            "a container below the header minimum must be rejected");

    koi::NnueNetwork future = koi::NnueNetwork::synthetic_v2();
    future.manifest.version = 5;
    const auto future_result = koi::NnueLoader::serialize(future);
    require(!future_result.has_value() &&
                future_result.error().code == koi::NnueErrorCode::unsupported_version,
            "future container versions must be rejected");

    koi::NnueNetwork v3 = koi::NnueNetwork::synthetic_v2();
    v3.manifest.version = koi::kKoiNnuePerspectiveV3FormatVersion;
    v3.hidden_shift = 7;
    v3.bottleneck_shift = 7;
    v3.output_shift = 7;
    const auto v3_encoded = koi::NnueLoader::serialize(v3);
    require(v3_encoded.has_value(), "v3 corruption fixture must serialize");
    std::vector<std::uint8_t> v3_corrupt_shift = *v3_encoded;
    v3_corrupt_shift[28] = 21;
    const auto v3_corrupt_shift_result = koi::NnueLoader::load(v3_corrupt_shift);
    require(!v3_corrupt_shift_result.has_value() &&
                v3_corrupt_shift_result.error().code == koi::NnueErrorCode::malformed_manifest,
            "v3 shifts above the named limit must be rejected");

    const koi::test::TempDirectory scratch;
    const auto missing = koi::NnueLoader::load_file(scratch.file("koi-missing.nnue"));
    require(!missing.has_value() && missing.error().code == koi::NnueErrorCode::io_error,
            "a missing NNUE file must report an IO error");
}

void verify_incremental_against_reference(koi::NnueWorker& incremental,
                                          koi::NnueWorker& reference,
                                          const koi::GameState& state) {
    const koi::Color mover = state.side_to_move();
    const int incremental_score = incremental.evaluate(state, mover);
    const int reference_score = reference.evaluate(state, mover);
    require(incremental_score == reference_score,
            "incremental v4 score must match a full recompute: " +
                std::to_string(incremental_score) + " != " +
                std::to_string(reference_score));
    require(incremental.accumulator().values == reference.accumulator().values &&
                incremental.accumulator().bottleneck_values ==
                    reference.accumulator().bottleneck_values,
            "incremental v4 accumulators must match a full recompute");
    require(incremental.evaluate(state, koi::opposite(mover)) == -incremental_score,
            "incremental v4 evaluation must mirror the requested perspective");
}

void walk_incremental_game(const std::string_view fen, const int plies,
                           const std::uint64_t seed) {
    const auto parsed = koi::GameState::from_fen(fen);
    require(parsed.has_value(), "incremental fixture FEN must parse");
    koi::GameState state = *parsed;
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    koi::NnueWorker incremental(weights);
    koi::NnueWorker reference(weights);
    std::uint64_t rng = seed;
    const auto next = [&rng]() {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng;
    };

    verify_incremental_against_reference(incremental, reference, state);
    const std::uint64_t fallbacks = incremental.incremental_fallback_count();

    std::vector<bool> null_steps;
    for (int ply = 0; ply < plies; ++ply) {
        if ((next() % 7U) == 0U) {
            const std::uint64_t parent_key = state.position_key();
            if (!state.make_null_move()) {
                break;
            }
            incremental.on_make_null_move(state, ply, parent_key);
            null_steps.push_back(true);
            verify_incremental_against_reference(incremental, reference, state);
            continue;
        }
        const std::vector<koi::MoveMetadata> moves = state.legal_moves_with_metadata();
        if (moves.empty()) {
            break;
        }
        const koi::MoveMetadata& metadata =
            moves[static_cast<std::size_t>(next() % moves.size())];
        const std::uint64_t parent_key = state.position_key();
        require(state.make_search_move(metadata), "incremental fixture move must be legal");
        incremental.on_make_move(state, metadata, ply, parent_key);
        null_steps.push_back(false);
        verify_incremental_against_reference(incremental, reference, state);
    }

    for (int index = static_cast<int>(null_steps.size()) - 1; index >= 0; --index) {
        const int child_ply = index + 1;
        if (null_steps[static_cast<std::size_t>(index)]) {
            incremental.on_unmake_null_move(child_ply);
            require(state.unmake_null_move(), "incremental fixture null move must unmake");
        } else {
            incremental.on_unmake_move(child_ply);
            state.unmake_move();
        }
        verify_incremental_against_reference(incremental, reference, state);
    }

    require(incremental.incremental_fallback_count() == fallbacks,
            "provided hooks must never force a full incremental fallback");
}

void test_v4_incremental_matches_full_recompute() {
    walk_incremental_game("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 28,
                          0xA5A5A5A5ULL);
    walk_incremental_game("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3", 20,
                          0x123456789ULL);
    walk_incremental_game("4k3/P6p/8/8/8/8/7P/4K3 w - - 0 1", 16, 0xDEADBEEFULL);
    walk_incremental_game("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", 12, 0xC0FFEEULL);
}

void test_v4_incremental_handles_king_bucket_crossing() {
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    koi::GameState state = koi::GameState::startpos();
    koi::NnueWorker incremental(weights);
    koi::NnueWorker reference(weights);
    verify_incremental_against_reference(incremental, reference, state);
    const std::uint64_t fallbacks = incremental.incremental_fallback_count();

    int ply = 0;
    for (const std::string_view move_text :
         {"d2d4", "e7e5", "e1d2", "e8e7", "d2d3", "e7d6"}) {
        const auto move = koi::Move::parse_uci(move_text);
        require(move.has_value(), "king-bucket fixture move must parse");
        const auto metadata = state.describe_move(*move);
        require(metadata.has_value(), "king-bucket fixture move must describe");
        const std::uint64_t parent_key = state.position_key();
        require(state.make_search_move(*metadata), "king-bucket fixture move must be legal");
        incremental.on_make_move(state, *metadata, ply, parent_key);
        ++ply;
        verify_incremental_against_reference(incremental, reference, state);
    }

    for (int index = ply - 1; index >= 0; --index) {
        incremental.on_unmake_move(index + 1);
        state.unmake_move();
        verify_incremental_against_reference(incremental, reference, state);
    }

    require(incremental.incremental_fallback_count() == fallbacks,
            "a king-bucket crossing must refresh in place without a full fallback");
}

void test_v4_incremental_recovers_from_skipped_hooks() {
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    koi::GameState state = koi::GameState::startpos();
    koi::NnueWorker incremental(weights);
    koi::NnueWorker reference(weights);
    verify_incremental_against_reference(incremental, reference, state);
    const std::uint64_t after_root = incremental.incremental_fallback_count();

    const auto move = koi::Move::parse_uci("e2e4");
    require(move.has_value(), "recovery fixture move must parse");
    const auto metadata = state.describe_move(*move);
    require(metadata.has_value(), "recovery fixture move must describe");
    require(state.make_search_move(*metadata), "recovery fixture move must be legal");
    verify_incremental_against_reference(incremental, reference, state);
    require(incremental.incremental_fallback_count() == after_root + 1,
            "a skipped make hook must fall back to a full recompute");

    state.unmake_move();
    verify_incremental_against_reference(incremental, reference, state);
    require(incremental.incremental_fallback_count() == after_root + 2,
            "a skipped make hook must also fall back after unmaking");
}

void test_v1_v2_serialization_rejects_nonzero_shifts() {
    koi::NnueNetwork shifted = koi::NnueNetwork::synthetic_v2();
    shifted.output_shift = 5;
    const auto rejected = koi::NnueLoader::serialize(shifted);
    require(!rejected.has_value() &&
                rejected.error().code == koi::NnueErrorCode::malformed_manifest,
            "v1/v2 serialization must reject nonzero shifts instead of dropping them");

    const auto encoded = koi::NnueLoader::serialize(koi::NnueNetwork::synthetic_v2());
    require(encoded.has_value(), "valid v2 networks still serialize");
    const auto decoded = koi::NnueLoader::load(*encoded);
    require(decoded.has_value(), "valid v2 containers still load");
    const auto repeated = koi::NnueLoader::serialize(*decoded);
    require(repeated.has_value() && *repeated == *encoded,
            "v2 round trips must stay byte-identical");
}

void test_invalid_in_memory_network_uses_the_classical_fallback() {
    koi::NnueNetwork broken = koi::NnueNetwork::synthetic_v4();
    broken.manifest.layer_sizes[1] = 31;
    const auto weights = std::make_shared<const koi::NnueNetwork>(std::move(broken));
    const koi::NnueEvaluator evaluator(weights);
    const koi::GameState state = require_state("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    const koi::ClassicalEvaluator classical;
    require(evaluator.evaluate(state, state.side_to_move()) ==
                classical.evaluate(state, state.side_to_move()),
            "invalid in-memory networks must fall back to classical evaluation");
    require(evaluator.create_worker() == nullptr,
            "invalid in-memory networks must not create worker state");
}

void test_one_shot_evaluation_matches_stateless_inference() {
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    const koi::NnueEvaluator evaluator(weights);
    const koi::GameState state =
        require_state("r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4");
    koi::NnueWorker worker(weights);
    const koi::EvaluationFeatures features = koi::EvaluationFeatureExtractor::extract(state);
    require(evaluator.evaluate(state, state.side_to_move()) ==
                worker.evaluate(features, state.side_to_move()),
            "one-shot NnueEvaluator::evaluate must match stateless feature inference");
}

void play_scripted_incremental(const std::string& fen,
                               std::initializer_list<std::string_view> moves) {
    const auto weights =
        std::make_shared<const koi::NnueNetwork>(koi::NnueNetwork::synthetic_v4());
    koi::GameState state = require_state(fen);
    koi::NnueWorker incremental(weights);
    koi::NnueWorker reference(weights);
    verify_incremental_against_reference(incremental, reference, state);
    const std::uint64_t fallbacks = incremental.incremental_fallback_count();

    int ply = 0;
    for (const std::string_view move_text : moves) {
        const auto move = koi::Move::parse_uci(move_text);
        require(move.has_value(),
                std::string("scripted incremental move must parse: ") + std::string(move_text));
        const auto metadata = state.describe_move(*move);
        require(metadata.has_value(),
                std::string("scripted incremental move must describe: ") + std::string(move_text) +
                    " in " + fen);
        const std::uint64_t parent_key = state.position_key();
        require(state.make_search_move(*metadata),
                std::string("scripted incremental move must be legal: ") + std::string(move_text) +
                    " in " + fen);
        incremental.on_make_move(state, *metadata, ply, parent_key);
        ++ply;
        verify_incremental_against_reference(incremental, reference, state);
    }

    for (int index = ply - 1; index >= 0; --index) {
        incremental.on_unmake_move(index + 1);
        state.unmake_move();
        verify_incremental_against_reference(incremental, reference, state);
    }

    require(incremental.incremental_fallback_count() == fallbacks,
            "scripted special moves must not need a full refresh");
}

void test_v4_incremental_scripted_special_moves() {
    // A castled white rook lands on f1/d1 and attacks the black castling
    // squares, so each side's castles are scripted from its own fixture.
    play_scripted_incremental("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", {"e1g1"});
    play_scripted_incremental("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", {"e1c1"});
    play_scripted_incremental("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", {"e8g8"});
    play_scripted_incremental("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1", {"e8c8"});
    play_scripted_incremental("rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
                              {"e5f6"});
    play_scripted_incremental("rnbqkbnr/pppp1ppp/8/8/3Pp3/8/PPP1PPPP/RNBQKBNR b KQkq d3 0 3",
                              {"e4d3"});
    play_scripted_incremental("n3k3/1P6/8/8/8/8/8/4K3 w - - 0 1", {"b7a8q"});
}

} // namespace

// Test-only seam: serialize a deterministic v4 fixture network and print the
// C++ sparse indices and integer scores for three reference positions.  The
// Python trainer tests invoke this through KOI_NNUE_BOUNDARY_EXE so both
// implementations are compared against the same fixture.
void emit_v4_fixture(const std::filesystem::path& output_path) {
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v4();
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    network.hidden_shift = 7;
    network.output_shift = 12;
    network.feature_weights.resize(input_units * hidden_units);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] =
            static_cast<std::int16_t>(static_cast<std::int32_t>(index * 37 % 2001) - 1000);
    }
    network.hidden_bias.resize(hidden_units);
    for (std::size_t h = 0; h < hidden_units; ++h) {
        network.hidden_bias[h] = static_cast<std::int32_t>(h * 97 % 251) - 125;
    }
    network.bottleneck_weights.resize(output_buckets * (hidden_units / 2));
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] =
            static_cast<std::int8_t>(static_cast<std::int32_t>(index * 29 % 255) - 127);
    }
    network.bottleneck_bias.resize(output_buckets);
    for (std::size_t bucket = 0; bucket < output_buckets; ++bucket) {
        network.bottleneck_bias[bucket] = static_cast<std::int32_t>(bucket * 13) - 50;
    }
    const auto encoded = koi::NnueLoader::serialize(network);
    if (!encoded.has_value()) {
        std::fputs("v4 fixture serialization failed\n", stderr);
        std::exit(2);
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    if (!output.good()) {
        std::fputs("v4 fixture container could not be written\n", stderr);
        std::exit(2);
    }
    output.close();

    const std::array<std::string_view, 3> fens{{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1",
        "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
    }};
    const auto weights = std::make_shared<const koi::NnueNetwork>(network);
    koi::NnueWorker worker(weights);
    std::printf("{\"hidden_units\":%u,\"hidden_shift\":%u,\"output_shift\":%u,\"positions\":[",
                network.manifest.layer_sizes[1], unsigned(network.hidden_shift),
                unsigned(network.output_shift));
    bool first = true;
    for (const std::string_view fen : fens) {
        const koi::GameState state = require_state(fen);
        const koi::EvaluationFeatures features =
            koi::EvaluationFeatureExtractor::extract(state);
        const koi::NnueSparseFeaturesV4 sparse =
            koi::EvaluationFeatureExtractor::encode_sparse_v4(features);
        const int score = worker.evaluate(features, features.position.side_to_move);
        std::printf("%s{\"fen\":\"%s\",\"score\":%d,\"indices\":[", first ? "" : ",",
                    std::string(fen).c_str(), score);
        first = false;
        for (std::size_t index = 0; index < sparse.count; ++index) {
            std::printf("%s%u", index == 0 ? "" : ",", unsigned(sparse.indices[index]));
        }
        std::printf("]}");
    }
    std::printf("]}\n");
}

int main(int argc, char** argv) {
    if (argc > 2 && argv[1] != nullptr &&
        std::string_view(argv[1]) == "--emit-v4-fixture") {
        emit_v4_fixture(std::filesystem::path(argv[2]));
        return 0;
    }
    if (argc > 1 && argv[1] != nullptr) {
        external_container_path = std::filesystem::path(argv[1]);
    }

    const std::vector<koi::test::TestCase> tests{
        {"NNUE container", test_container_round_trip_and_validation},
        {"NNUE layer shape", test_container_rejects_nonstandard_layer_shape},
        {"NNUE fallback", test_malformed_or_absent_network_uses_classical_fallback},
        {"NNUE worker isolation", test_workers_keep_immutable_weights_and_private_accumulators},
        {"NNUE v2 features", test_v2_feature_vector_has_stable_king_and_pawn_context},
        {"NNUE v2 container", test_v2_container_round_trip_is_deterministic},
        {"NNUE v2 validation", test_v2_manifest_validation_rejects_mismatches},
        {"NNUE v2 inference", test_v2_golden_vector_and_inference_paths},
        {"NNUE AVX2 wide accumulation", test_avx2_compatible_path_preserves_wide_accumulation},
        {"NNUE invalid worker guard", test_invalid_network_does_not_allocate_worker_accumulators},
        {"NNUE v3 shifts", test_v3_shift_explicit_inference_and_container},
        {"NNUE v4 golden score", test_v4_golden_pair_product_score},
        {"NNUE v4 reference", test_v4_scalar_matches_independent_reference},
        {"NNUE v4 path parity", test_v4_inference_paths_agree_on_random_weights},
        {"NNUE v4 piece bucket", test_v4_piece_count_bucket_selects_the_head},
        {"NNUE v4 wide accumulation", test_v4_wide_accumulation_preserves_clipping},
        {"NNUE v4 container", test_v4_container_round_trip_and_validation},
        {"NNUE v4 validation", test_v4_manifest_validation_rejects_mismatches},
        {"NNUE v4 corruption", test_v4_container_rejects_corruption_and_legacy_versions},
        {"NNUE v4 incremental walk", test_v4_incremental_matches_full_recompute},
        {"NNUE v4 incremental king bucket", test_v4_incremental_handles_king_bucket_crossing},
        {"NNUE v4 incremental recovery", test_v4_incremental_recovers_from_skipped_hooks},
        {"NNUE v4 incremental special moves", test_v4_incremental_scripted_special_moves},
        {"NNUE v2 shifts rejected", test_v1_v2_serialization_rejects_nonzero_shifts},
        {"NNUE invalid fallback", test_invalid_in_memory_network_uses_the_classical_fallback},
        {"NNUE one-shot stateless", test_one_shot_evaluation_matches_stateless_inference},
        {"external NNUE container", test_external_v2_container},
    };
    return koi::test::run_tests(tests, argc, argv);
}
