#include "koi/policy_value_model.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <new>
#include <utility>

namespace koi {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'K', 'O', 'I', 'P', 'V', '1', 0, 0};
constexpr std::size_t kStateEmbeddingFloatCount =
    kPolicyValueFeatureCount * kPolicyValueHiddenSize;
constexpr std::size_t kStateBiasFloatCount = kPolicyValueHiddenSize;
constexpr std::size_t kFromEmbeddingFloatCount = 64 * kPolicyValueHiddenSize;
constexpr std::size_t kToEmbeddingFloatCount = 64 * kPolicyValueHiddenSize;
constexpr std::size_t kPromotionEmbeddingFloatCount = 5 * kPolicyValueHiddenSize;
constexpr std::size_t kValueWeightFloatCount = kPolicyValueHiddenSize * 3;
constexpr std::size_t kValueBiasFloatCount = 3;

constexpr std::size_t kStateEmbeddingOffset = 0;
constexpr std::size_t kStateBiasOffset =
    kStateEmbeddingOffset + kStateEmbeddingFloatCount;
constexpr std::size_t kFromEmbeddingOffset = kStateBiasOffset + kStateBiasFloatCount;
constexpr std::size_t kToEmbeddingOffset =
    kFromEmbeddingOffset + kFromEmbeddingFloatCount;
constexpr std::size_t kPromotionEmbeddingOffset =
    kToEmbeddingOffset + kToEmbeddingFloatCount;
constexpr std::size_t kPolicyBiasOffset =
    kPromotionEmbeddingOffset + kPromotionEmbeddingFloatCount;
constexpr std::size_t kValueWeightsOffset = kPolicyBiasOffset + 1;
constexpr std::size_t kValueBiasOffset = kValueWeightsOffset + kValueWeightFloatCount;
constexpr std::size_t kWeightFloatCount = kValueBiasOffset + kValueBiasFloatCount;
constexpr std::size_t kPayloadSize = kWeightFloatCount * sizeof(float);
constexpr std::size_t kFileSize = kPolicyValueHeaderSize + kPayloadSize;

static_assert(sizeof(float) == 4);
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(kFileSize == 9'472'312);

[[nodiscard]] PolicyValueModelError make_error(
    const PolicyValueModelErrorCode code, std::string message) {
    return PolicyValueModelError{code, std::move(message)};
}

[[nodiscard]] constexpr std::uint32_t read_u32(
    const std::span<const std::uint8_t> bytes, const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] constexpr std::uint64_t read_u64(
    const std::span<const std::uint8_t> bytes, const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xedb88320U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto kCrc32Table = make_crc32_table();

[[nodiscard]] std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t checksum = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        const std::uint8_t index = static_cast<std::uint8_t>(checksum ^ byte);
        checksum = kCrc32Table[index] ^ (checksum >> 8U);
    }
    return checksum ^ 0xffffffffU;
}

[[nodiscard]] bool is_magic(const std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= kMagic.size() &&
        std::equal(kMagic.begin(), kMagic.end(), bytes.begin());
}

[[nodiscard]] std::expected<std::size_t, PolicyValueModelError> promotion_index(
    const Promotion promotion) {
    switch (promotion) {
    case Promotion::none:
        return 0;
    case Promotion::queen:
        return 1;
    case Promotion::rook:
        return 2;
    case Promotion::bishop:
        return 3;
    case Promotion::knight:
        return 4;
    }
    return std::unexpected(make_error(
        PolicyValueModelErrorCode::invalid_actions, "legal move has an invalid promotion value"));
}

} // namespace

std::expected<PolicyValueModel, PolicyValueModelError> PolicyValueModel::load(
    const std::span<const std::uint8_t> container) {
    if (container.size() < kPolicyValueHeaderSize) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::truncated_header, "truncated policy/value model header"));
    }
    if (!is_magic(container)) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::bad_magic, "invalid policy/value model magic"));
    }

    const std::uint32_t version = read_u32(container, 8);
    const std::uint32_t feature_schema = read_u32(container, 12);
    const std::uint32_t action_encoding = read_u32(container, 16);
    const std::uint32_t hidden_size = read_u32(container, 20);
    const std::uint32_t feature_count = read_u32(container, 24);
    const std::uint64_t payload_size = read_u64(container, 28);
    const std::uint32_t expected_crc = read_u32(container, 36);

    if (version != kPolicyValueModelVersion) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::unsupported_version,
            "unsupported policy/value model version: " + std::to_string(version)));
    }
    if (feature_schema != kPolicyValueFeatureSchemaId) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::unsupported_feature_schema,
            "unsupported policy/value feature schema ID: " + std::to_string(feature_schema)));
    }
    if (action_encoding != kPolicyValueActionEncodingId) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::unsupported_action_encoding,
            "unsupported policy/value action encoding ID: " + std::to_string(action_encoding)));
    }
    if (hidden_size != kPolicyValueHiddenSize || feature_count != kPolicyValueFeatureCount) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_dimensions,
            "unsupported policy/value model dimensions"));
    }
    if (payload_size != kPayloadSize || container.size() != kFileSize) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_file_size,
            "policy/value payload length does not match the v1 layout"));
    }

    const auto payload = container.subspan(kPolicyValueHeaderSize);
    if (crc32(payload) != expected_crc) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::checksum_mismatch, "policy/value model CRC32 mismatch"));
    }

    std::vector<float> weights;
    try {
        weights.reserve(kWeightFloatCount);
        for (std::size_t offset = 0; offset < payload.size(); offset += sizeof(float)) {
            const float value = std::bit_cast<float>(read_u32(payload, offset));
            if (!std::isfinite(value)) {
                return std::unexpected(make_error(
                    PolicyValueModelErrorCode::non_finite_weight,
                    "policy/value model contains a nonfinite float32 weight"));
            }
            weights.push_back(value);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::allocation_failure,
            "unable to allocate memory for policy/value model weights"));
    }
    if (weights.size() != kWeightFloatCount) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_file_size,
            "policy/value model weight payload is incomplete"));
    }
    return PolicyValueModel(std::move(weights));
}

std::expected<PolicyValueModel, PolicyValueModelError> PolicyValueModel::load_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::io_error, "unable to open policy/value model"));
    }
    const std::streampos end = input.tellg();
    if (end < 0) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::io_error, "unable to size policy/value model"));
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size < kPolicyValueHeaderSize) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::truncated_header,
            "truncated policy/value model header"));
    }
    if (size != kFileSize) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_file_size,
            "policy/value model file does not match the v1 container size"));
    }

    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(kFileSize);
    } catch (const std::bad_alloc&) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::allocation_failure,
            "unable to allocate memory for policy/value model file"));
    }
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::io_error, "unable to read policy/value model"));
    }
    return load(bytes);
}

std::expected<float, PolicyValueModelError> PolicyValueModel::evaluate(
    const NnueSparseFeaturesV5& sparse_features, const Color side_to_move,
    const std::span<const Move> legal_moves, const std::span<float> priors,
    std::array<float, 3>& wdl) const {
    if (side_to_move != Color::white && side_to_move != Color::black) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_actions, "invalid side-to-move color"));
    }
    if (sparse_features.count > sparse_features.indices.size()) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_features, "v5 sparse feature count exceeds its capacity"));
    }
    std::uint16_t previous_feature = 0;
    for (std::size_t index = 0; index < sparse_features.count; ++index) {
        const std::uint16_t feature = sparse_features.indices[index];
        if (feature >= kPolicyValueFeatureCount ||
            (index != 0 && feature <= previous_feature)) {
            return std::unexpected(make_error(
                PolicyValueModelErrorCode::invalid_features,
                "v5 sparse features must be in-range, unique, and strictly increasing"));
        }
        previous_feature = feature;
    }
    if (legal_moves.empty()) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_actions, "legal move list must not be empty"));
    }
    if (legal_moves.size() > kMaximumLegalMoves) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::invalid_actions, "legal move list exceeds Koi's native capacity"));
    }
    if (priors.size() != legal_moves.size()) {
        return std::unexpected(make_error(
            PolicyValueModelErrorCode::output_size_mismatch,
            "policy prior output size must match the legal move list"));
    }

    std::array<double, kPolicyValueHiddenSize> hidden{};
    for (std::size_t unit = 0; unit < kPolicyValueHiddenSize; ++unit) {
        hidden[unit] = static_cast<double>(weights_[kStateBiasOffset + unit]);
    }
    for (std::size_t feature_offset = 0;
         feature_offset < sparse_features.count; ++feature_offset) {
        const std::size_t row =
            kStateEmbeddingOffset +
            static_cast<std::size_t>(sparse_features.indices[feature_offset]) *
                kPolicyValueHiddenSize;
        for (std::size_t unit = 0; unit < kPolicyValueHiddenSize; ++unit) {
            hidden[unit] += static_cast<double>(weights_[row + unit]);
        }
    }
    for (double& activation : hidden) {
        activation = std::max(0.0, activation);
    }

    std::array<double, kMaximumLegalMoves> logits{};
    double maximum_logit = -std::numeric_limits<double>::infinity();
    for (std::size_t move_index = 0; move_index < legal_moves.size(); ++move_index) {
        const Move move = legal_moves[move_index];
        if (move.is_no_move() || move.from().index() >= Square::kInvalid ||
            move.to().index() >= Square::kInvalid || move.from() == move.to()) {
            return std::unexpected(make_error(
                PolicyValueModelErrorCode::invalid_actions,
                "legal move list contains an invalid native move"));
        }
        const auto promotion = promotion_index(move.promotion());
        if (!promotion.has_value()) {
            return std::unexpected(promotion.error());
        }
        std::size_t from = move.from().index();
        std::size_t to = move.to().index();
        if (side_to_move == Color::black) {
            from ^= 56U;
            to ^= 56U;
        }
        double logit = static_cast<double>(weights_[kPolicyBiasOffset]);
        const std::size_t from_row = kFromEmbeddingOffset + from * kPolicyValueHiddenSize;
        const std::size_t to_row = kToEmbeddingOffset + to * kPolicyValueHiddenSize;
        const std::size_t promotion_row =
            kPromotionEmbeddingOffset + promotion.value() * kPolicyValueHiddenSize;
        for (std::size_t unit = 0; unit < kPolicyValueHiddenSize; ++unit) {
            const double action_weight =
                static_cast<double>(weights_[from_row + unit]) +
                static_cast<double>(weights_[to_row + unit]) +
                static_cast<double>(weights_[promotion_row + unit]);
            logit += hidden[unit] * action_weight;
        }
        logits[move_index] = logit;
        maximum_logit = std::max(maximum_logit, logit);
    }

    double policy_denominator = 0.0;
    for (std::size_t index = 0; index < legal_moves.size(); ++index) {
        const double exponential = std::exp(logits[index] - maximum_logit);
        logits[index] = exponential;
        policy_denominator += exponential;
    }
    for (std::size_t index = 0; index < legal_moves.size(); ++index) {
        priors[index] = static_cast<float>(logits[index] / policy_denominator);
    }

    std::array<double, 3> value_logits{};
    for (std::size_t result = 0; result < value_logits.size(); ++result) {
        double logit = static_cast<double>(weights_[kValueBiasOffset + result]);
        for (std::size_t unit = 0; unit < kPolicyValueHiddenSize; ++unit) {
            logit += hidden[unit] * static_cast<double>(
                weights_[kValueWeightsOffset + unit * value_logits.size() + result]);
        }
        value_logits[result] = logit;
    }
    const double maximum_value_logit =
        *std::max_element(value_logits.begin(), value_logits.end());
    double value_denominator = 0.0;
    for (double& logit : value_logits) {
        logit = std::exp(logit - maximum_value_logit);
        value_denominator += logit;
    }
    for (std::size_t result = 0; result < wdl.size(); ++result) {
        wdl[result] = static_cast<float>(value_logits[result] / value_denominator);
    }
    return static_cast<float>(
        value_logits[0] / value_denominator - value_logits[2] / value_denominator);
}

} // namespace koi
