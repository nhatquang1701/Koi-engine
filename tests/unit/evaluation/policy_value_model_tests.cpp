#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#include "koi/evaluation_features.hpp"
#include "koi/move.hpp"
#include "koi/policy_value_model.hpp"
#include "koi_test_support.hpp"

namespace {

using koi::test::require;

constexpr std::size_t kHeaderSize = 40;
constexpr std::size_t kHiddenSize = 64;
constexpr std::size_t kFeatureCount = 36'864;
constexpr std::size_t kStateEmbeddingFloats = kFeatureCount * kHiddenSize;
constexpr std::size_t kStateBiasFloats = kHiddenSize;
constexpr std::size_t kFromEmbeddingFloats = 64 * kHiddenSize;
constexpr std::size_t kToEmbeddingFloats = 64 * kHiddenSize;
constexpr std::size_t kPromotionEmbeddingFloats = 5 * kHiddenSize;
constexpr std::size_t kFirstFiveWeightFloats =
    kStateEmbeddingFloats + kStateBiasFloats + kFromEmbeddingFloats +
    kToEmbeddingFloats + kPromotionEmbeddingFloats;
constexpr std::size_t kValueWeightsFloatOffset = kFirstFiveWeightFloats + 1;
constexpr std::size_t kPayloadBytes =
    (kFirstFiveWeightFloats + 1 + kHiddenSize * 3 + 3) * 4;

void write_u32(std::vector<std::uint8_t>& bytes, const std::size_t offset,
               const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void write_u64(std::vector<std::uint8_t>& bytes, const std::size_t offset,
               const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

void write_f32(std::vector<std::uint8_t>& bytes, const std::size_t float_index,
               const float value) {
    write_u32(bytes, kHeaderSize + float_index * 4, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t checksum = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        checksum ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (checksum & 1U);
            checksum = (checksum >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return checksum ^ 0xffffffffU;
}

void refresh_payload_crc(std::vector<std::uint8_t>& bytes) {
    write_u32(bytes, 36, crc32(std::span<const std::uint8_t>(bytes).subspan(kHeaderSize)));
}

[[nodiscard]] std::vector<std::uint8_t> zero_model_container() {
    std::vector<std::uint8_t> bytes(kHeaderSize + kPayloadBytes, 0);
    constexpr std::array<std::uint8_t, 8> magic{'K', 'O', 'I', 'P', 'V', '1', 0, 0};
    std::memcpy(bytes.data(), magic.data(), magic.size());
    write_u32(bytes, 8, 1); // model version
    write_u32(bytes, 12, 5); // feature schema
    write_u32(bytes, 16, 1); // action encoding
    write_u32(bytes, 20, static_cast<std::uint32_t>(kHiddenSize));
    write_u32(bytes, 24, static_cast<std::uint32_t>(kFeatureCount));
    write_u64(bytes, 28, kPayloadBytes);
    refresh_payload_crc(bytes);
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t> pattern_model_container() {
    std::vector<std::uint8_t> bytes = zero_model_container();
    constexpr std::size_t state_bias_offset = kStateEmbeddingFloats;
    constexpr std::size_t from_embedding_offset =
        state_bias_offset + kStateBiasFloats;

    // The first hidden unit is active. Canonical e2/e7-from moves receive a
    // larger policy logit, and the WDL head has logits [1, 0, -1].
    write_f32(bytes, state_bias_offset, 1.0F);
    write_f32(bytes, from_embedding_offset + 12 * kHiddenSize, 1.0F);
    write_f32(bytes, kValueWeightsFloatOffset, 1.0F);
    write_f32(bytes, kValueWeightsFloatOffset + 2, -1.0F);
    refresh_payload_crc(bytes);
    return bytes;
}

[[nodiscard]] koi::Move require_move(const std::string_view uci) {
    const auto move = koi::Move::parse_uci(uci);
    require(move.has_value(), "policy/value test move must parse");
    return *move;
}

[[nodiscard]] bool near(const float actual, const double expected,
                        const double tolerance = 1e-5) noexcept {
    return std::abs(static_cast<double>(actual) - expected) <= tolerance;
}

void test_v1_model_load_matches_cpu_reference_and_black_action_mirror() {
    auto encoded = pattern_model_container();
    auto loaded = koi::PolicyValueModel::load(encoded);
    require(loaded.has_value(), "the strict v1 C++ loader must accept the Python-compatible container");

    koi::NnueSparseFeaturesV5 sparse;
    std::array<float, 2> white_priors{};
    std::array<float, 3> white_wdl{};
    const std::array<koi::Move, 2> white_moves{
        require_move("d2d4"), require_move("e2e4")};
    const auto white_value = loaded->evaluate(
        sparse, koi::Color::white, white_moves, white_priors, white_wdl);
    require(white_value.has_value(), "the white-to-move CPU inference call must succeed");
    require(near(white_priors[0], 0.2689414214) &&
                near(white_priors[1], 0.7310585786),
            "policy probabilities must be normalized in the caller's legal-move order");
    require(near(white_wdl[0], 0.6652409558) && near(white_wdl[1], 0.2447284711) &&
                near(white_wdl[2], 0.0900305732) && near(*white_value, 0.5752103826),
            "the WDL head and STM value must match the CPU reference calculation");

    std::array<float, 2> black_priors{};
    std::array<float, 3> black_wdl{};
    const std::array<koi::Move, 2> black_moves{
        require_move("d7d5"), require_move("e7e5")};
    const auto black_value = loaded->evaluate(
        sparse, koi::Color::black, black_moves, black_priors, black_wdl);
    require(black_value.has_value(), "the black-to-move CPU inference call must succeed");
    require(near(black_priors[0], white_priors[0]) &&
                near(black_priors[1], white_priors[1]),
            "black action squares must mirror vertically into the canonical policy coordinates");
    require(near(*black_value, *white_value) && near(black_wdl[0], white_wdl[0]) &&
                near(black_wdl[1], white_wdl[1]) && near(black_wdl[2], white_wdl[2]),
            "WDL and value must be reported from the side-to-move perspective");
}

void test_v1_model_load_rejects_header_and_file_size_mismatches() {
    auto bytes = zero_model_container();
    bytes[0] = 'X';
    auto bad_magic = koi::PolicyValueModel::load(bytes);
    require(!bad_magic && bad_magic.error().code == koi::PolicyValueModelErrorCode::bad_magic,
            "an unknown magic must be rejected");

    bytes = zero_model_container();
    write_u32(bytes, 8, 2);
    auto bad_version = koi::PolicyValueModel::load(bytes);
    require(!bad_version && bad_version.error().code ==
                koi::PolicyValueModelErrorCode::unsupported_version,
            "unsupported model versions must be rejected");

    bytes = zero_model_container();
    write_u32(bytes, 12, 6);
    auto bad_features = koi::PolicyValueModel::load(bytes);
    require(!bad_features && bad_features.error().code ==
                koi::PolicyValueModelErrorCode::unsupported_feature_schema,
            "unsupported feature schemas must be rejected");

    bytes = zero_model_container();
    write_u32(bytes, 16, 2);
    auto bad_actions = koi::PolicyValueModel::load(bytes);
    require(!bad_actions && bad_actions.error().code ==
                koi::PolicyValueModelErrorCode::unsupported_action_encoding,
            "unsupported action encodings must be rejected");

    bytes = zero_model_container();
    write_u32(bytes, 20, 32);
    auto bad_dimensions = koi::PolicyValueModel::load(bytes);
    require(!bad_dimensions && bad_dimensions.error().code ==
                koi::PolicyValueModelErrorCode::invalid_dimensions,
            "unsupported dimensions must be rejected");

    bytes = zero_model_container();
    write_u64(bytes, 28, kPayloadBytes + 4);
    auto bad_payload_length = koi::PolicyValueModel::load(bytes);
    require(!bad_payload_length && bad_payload_length.error().code ==
                koi::PolicyValueModelErrorCode::invalid_file_size,
            "a mismatched payload length must be rejected");

    bytes = zero_model_container();
    bytes.push_back(0);
    auto trailing_byte = koi::PolicyValueModel::load(bytes);
    require(!trailing_byte && trailing_byte.error().code ==
                koi::PolicyValueModelErrorCode::invalid_file_size,
            "trailing bytes must be rejected");

    bytes = zero_model_container();
    bytes.pop_back();
    auto truncated_payload = koi::PolicyValueModel::load(bytes);
    require(!truncated_payload && truncated_payload.error().code ==
                koi::PolicyValueModelErrorCode::invalid_file_size,
            "a truncated payload must be rejected");
}

void test_v1_model_load_rejects_crc_and_nonfinite_weights() {
    auto bytes = zero_model_container();
    bytes.back() = 1;
    auto bad_crc = koi::PolicyValueModel::load(bytes);
    require(!bad_crc && bad_crc.error().code ==
                koi::PolicyValueModelErrorCode::checksum_mismatch,
            "a payload with a stale CRC32 must be rejected");

    bytes = zero_model_container();
    write_u32(bytes, kHeaderSize, 0x7fc00000U); // IEEE-754 quiet NaN
    refresh_payload_crc(bytes);
    auto nonfinite = koi::PolicyValueModel::load(bytes);
    require(!nonfinite && nonfinite.error().code ==
                koi::PolicyValueModelErrorCode::non_finite_weight,
            "nonfinite tensor values must be rejected even with a valid CRC32");
}

void test_v1_model_inference_rejects_invalid_features_actions_and_outputs() {
    auto loaded = koi::PolicyValueModel::load(zero_model_container());
    require(loaded.has_value(), "the valid zero model must load for input validation tests");
    const std::array<koi::Move, 1> move{require_move("e2e4")};
    std::array<float, 1> prior{};
    std::array<float, 3> wdl{};
    koi::NnueSparseFeaturesV5 sparse;
    sparse.count = 1;
    sparse.indices[0] = static_cast<std::uint16_t>(kFeatureCount);
    auto out_of_range = loaded->evaluate(sparse, koi::Color::white, move, prior, wdl);
    require(!out_of_range && out_of_range.error().code ==
                koi::PolicyValueModelErrorCode::invalid_features,
            "out-of-range v5 feature indices must be rejected");

    sparse = {};
    sparse.count = 2;
    sparse.indices[0] = 7;
    sparse.indices[1] = 7;
    auto duplicate_feature = loaded->evaluate(sparse, koi::Color::white, move, prior, wdl);
    require(!duplicate_feature && duplicate_feature.error().code ==
                koi::PolicyValueModelErrorCode::invalid_features,
            "duplicate v5 feature indices must be rejected");

    sparse = {};
    const std::span<const koi::Move> no_moves;
    const std::span<float> no_priors;
    auto empty_actions = loaded->evaluate(sparse, koi::Color::white, no_moves, no_priors, wdl);
    require(!empty_actions && empty_actions.error().code ==
                koi::PolicyValueModelErrorCode::invalid_actions,
            "an empty legal-move list must be rejected");

    std::array<float, 2> wrong_size{};
    auto mismatched_output = loaded->evaluate(sparse, koi::Color::white, move, wrong_size, wdl);
    require(!mismatched_output && mismatched_output.error().code ==
                koi::PolicyValueModelErrorCode::output_size_mismatch,
            "the prior output must match the supplied legal-move count");
}

void test_v1_model_file_loader_reads_and_reports_missing_files() {
    const koi::test::TempDirectory temporary;
    const std::filesystem::path model_path = temporary.file("zero.kpv");
    const std::vector<std::uint8_t> bytes = zero_model_container();
    {
        std::ofstream output(model_path, std::ios::binary | std::ios::trunc);
        require(output.good(), "the temporary model file must open");
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        require(output.good(), "the temporary model file must write completely");
    }
    auto loaded = koi::PolicyValueModel::load_file(model_path);
    require(loaded.has_value(), "load_file must accept a valid v1 container");

    auto missing = koi::PolicyValueModel::load_file(temporary.file("missing.kpv"));
    require(!missing && missing.error().code == koi::PolicyValueModelErrorCode::io_error,
            "load_file must report a missing model without throwing");
}

} // namespace

int main(int argc, char** argv) {
    const std::array<koi::test::TestCase, 5> tests{{
        {"policy/value v1 CPU inference and symmetry",
         test_v1_model_load_matches_cpu_reference_and_black_action_mirror},
        {"policy/value v1 header and size rejection",
         test_v1_model_load_rejects_header_and_file_size_mismatches},
        {"policy/value v1 checksum and nonfinite rejection",
         test_v1_model_load_rejects_crc_and_nonfinite_weights},
        {"policy/value inference input validation",
         test_v1_model_inference_rejects_invalid_features_actions_and_outputs},
        {"policy/value v1 file loader", test_v1_model_file_loader_reads_and_reports_missing_files},
    }};
    return koi::test::run_tests(tests, argc, argv);
}
