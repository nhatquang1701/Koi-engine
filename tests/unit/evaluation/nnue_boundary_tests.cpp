#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/evaluation_features.hpp"
#include "koi/nnue.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::test::require;

koi::GameState require_state(std::string_view fen) {
    const auto state = koi::GameState::from_fen(fen);
    require(state.has_value(), "NNUE fixture must be valid: " + std::string(fen));
    return *state;
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

    const koi::EvaluatorSelection absent = koi::make_evaluator(std::nullopt);
    require(!absent.nnue_enabled && absent.evaluator &&
                absent.evaluator->evaluate(state, koi::Color::white) ==
                    classical.evaluate(state, koi::Color::white),
            "absent NNUE must select the classical evaluator");

    const std::filesystem::path malformed_path =
        std::filesystem::temp_directory_path() / "koi-task4-malformed.nnue";
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

    const std::filesystem::path valid_path =
        std::filesystem::temp_directory_path() / "koi-v2-opt-in.nnue";
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

void test_external_v2_container(const std::filesystem::path& path) {
    const auto decoded = koi::NnueLoader::load_file(path);
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

} // namespace

int main(int argc, char** argv) {
    try {
        test_container_round_trip_and_validation();
        std::cout << "PASS NNUE container\n";
        test_container_rejects_nonstandard_layer_shape();
        std::cout << "PASS NNUE layer shape\n";
        test_malformed_or_absent_network_uses_classical_fallback();
        std::cout << "PASS NNUE fallback\n";
        test_workers_keep_immutable_weights_and_private_accumulators();
        std::cout << "PASS NNUE worker isolation\n";
        test_v2_feature_vector_has_stable_king_and_pawn_context();
        std::cout << "PASS NNUE v2 features\n";
        test_v2_container_round_trip_is_deterministic();
        std::cout << "PASS NNUE v2 container\n";
        test_v2_manifest_validation_rejects_mismatches();
        std::cout << "PASS NNUE v2 validation\n";
        test_v2_golden_vector_and_inference_paths();
        std::cout << "PASS NNUE v2 inference\n";
        test_avx2_compatible_path_preserves_wide_accumulation();
        std::cout << "PASS NNUE AVX2 wide accumulation\n";
        test_invalid_network_does_not_allocate_worker_accumulators();
        std::cout << "PASS NNUE invalid worker guard\n";
        test_v3_shift_explicit_inference_and_container();
        std::cout << "PASS NNUE v3 shifts\n";
        if (argc > 1) {
            test_external_v2_container(argv[1]);
            std::cout << "PASS external NNUE container\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
