#include "koi/nnue.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include "koi/classical_evaluator.hpp"
#include "koi/cpu_features.hpp"

#if defined(_M_AVX2) || defined(__AVX2__)
#include <immintrin.h>
#define KOI_NNUE_COMPILED_AVX2 1
#else
#define KOI_NNUE_COMPILED_AVX2 0
#endif

namespace koi {
namespace {

constexpr std::size_t kContainerHeaderBytes = 72;

NnueError make_error(NnueErrorCode code, std::string message) {
    return NnueError{code, std::move(message)};
}

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned amount) noexcept {
    return (value >> amount) | (value << (32U - amount));
}

class Sha256 final {
public:
    Sha256() noexcept
        : state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U} {}

    void update(std::span<const std::uint8_t> bytes) noexcept {
        for (const std::uint8_t byte : bytes) {
            buffer_[buffer_size_++] = byte;
            if (buffer_size_ == buffer_.size()) {
                transform();
                bit_count_ += 512;
                buffer_size_ = 0;
            }
        }
    }

    [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
        const std::uint64_t original_bits = bit_count_ + buffer_size_ * 8U;
        buffer_[buffer_size_++] = 0x80U;
        if (buffer_size_ > 56) {
            while (buffer_size_ < buffer_.size()) {
                buffer_[buffer_size_++] = 0;
            }
            transform();
            buffer_size_ = 0;
        }
        while (buffer_size_ < 56) {
            buffer_[buffer_size_++] = 0;
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            buffer_[buffer_size_++] = static_cast<std::uint8_t>(original_bits >> shift);
        }
        transform();

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            digest[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
            digest[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
            digest[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
            digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
        }
        return digest;
    }

private:
    void transform() noexcept {
        static constexpr std::array<std::uint32_t, 64> kRoundConstants{
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
        };
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index) {
            const std::size_t offset = index * 4;
            schedule[index] = (static_cast<std::uint32_t>(buffer_[offset]) << 24) |
                (static_cast<std::uint32_t>(buffer_[offset + 1]) << 16) |
                (static_cast<std::uint32_t>(buffer_[offset + 2]) << 8) |
                static_cast<std::uint32_t>(buffer_[offset + 3]);
        }
        for (std::size_t index = 16; index < schedule.size(); ++index) {
            const std::uint32_t first = schedule[index - 15];
            const std::uint32_t second = schedule[index - 2];
            const std::uint32_t sigma0 = rotate_right(first, 7) ^ rotate_right(first, 18) ^ (first >> 3);
            const std::uint32_t sigma1 = rotate_right(second, 17) ^ rotate_right(second, 19) ^ (second >> 10);
            schedule[index] = schedule[index - 16] + sigma0 + schedule[index - 7] + sigma1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temporary1 = h + sigma1 + choose + kRoundConstants[index] + schedule[index];
            const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temporary2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t bit_count_ = 0;
};

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) noexcept {
    Sha256 hasher;
    hasher.update(bytes);
    return hasher.finish();
}

template <typename Integer>
void append_little_endian(std::vector<std::uint8_t>& output, Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned unsigned_value = static_cast<Unsigned>(value);
    for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
        output.push_back(static_cast<std::uint8_t>(unsigned_value >> (shift * 8U)));
    }
}

template <typename Integer>
[[nodiscard]] bool read_little_endian(std::span<const std::uint8_t> bytes,
                                      std::size_t& offset, Integer& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(Integer)) {
        return false;
    }
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned assembled = 0;
    for (std::size_t shift = 0; shift < sizeof(Integer); ++shift) {
        assembled |= static_cast<Unsigned>(bytes[offset++]) << (shift * 8U);
    }
    value = static_cast<Integer>(assembled);
    return true;
}

[[nodiscard]] std::optional<std::size_t> payload_size_for(const NnueManifest& manifest) noexcept {
    if (manifest.feature_set == kKoiNnueFeatureSet &&
        manifest.layer_sizes != kKoiNnueLayerSizes) {
        return std::nullopt;
    }
    if (manifest.feature_set == kKoiNnuePieceSquareKingPawnV2FeatureSet &&
        manifest.layer_sizes != kKoiNnuePieceSquareKingPawnV2LayerSizes) {
        return std::nullopt;
    }
    if (manifest.feature_set != kKoiNnueFeatureSet &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return std::nullopt;
    }
    const std::size_t hidden_units = manifest.layer_sizes[1];
    const std::size_t bottleneck_units = manifest.layer_sizes[2];
    const std::size_t actual_input_units = manifest.layer_sizes[0];
    return actual_input_units * hidden_units * sizeof(std::int16_t) +
        hidden_units * sizeof(std::int32_t) +
        hidden_units * bottleneck_units * sizeof(std::int8_t) +
        bottleneck_units * sizeof(std::int32_t) +
        bottleneck_units * sizeof(std::int8_t) + sizeof(std::int32_t);
}

[[nodiscard]] std::optional<NnueError> validate_manifest(const NnueManifest& manifest) {
    if (manifest.magic != kKoiNnueMagic) {
        return make_error(NnueErrorCode::bad_magic, "NNUE container magic is not KOI-NNUE");
    }
    if (manifest.version != kKoiNnueFormatVersion) {
        return make_error(NnueErrorCode::unsupported_version, "unsupported Koi NNUE container version");
    }
    if (manifest.feature_set != kKoiNnueFeatureSet &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return make_error(NnueErrorCode::unsupported_feature_set, "unsupported Koi NNUE feature set");
    }
    const NnueLayerSizes expected = manifest.feature_set == kKoiNnueFeatureSet ?
        kKoiNnueLayerSizes : kKoiNnuePieceSquareKingPawnV2LayerSizes;
    if (manifest.layer_sizes != expected) {
        return make_error(
            NnueErrorCode::invalid_dimensions,
            manifest.feature_set == kKoiNnueFeatureSet ?
                "NNUE v1 layer dimensions must be 768, 128, 32, 1" :
                "NNUE v2 layer dimensions must be 960, 256, 32, 1");
    }
    if (manifest.quantization != kKoiNnueQuantization) {
        return make_error(NnueErrorCode::unsupported_quantization,
                          "only int16/int8 Koi NNUE quantization is supported");
    }
    return std::nullopt;
}

[[nodiscard]] NnueError invalid_file_size_error() {
    return make_error(NnueErrorCode::invalid_file_size, "NNUE container size does not match its manifest");
}

[[nodiscard]] int clamp_score(std::int64_t score) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(
        score, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
}

[[nodiscard]] bool arrays_match_manifest(const NnueNetwork& network) noexcept {
    const auto& sizes = network.manifest.layer_sizes;
    return network.feature_weights.size() == static_cast<std::size_t>(sizes[0]) * sizes[1] &&
        network.hidden_bias.size() == sizes[1] &&
        network.bottleneck_weights.size() == static_cast<std::size_t>(sizes[1]) * sizes[2] &&
        network.bottleneck_bias.size() == sizes[2] &&
        network.output_weights.size() == sizes[2];
}

[[nodiscard]] bool avx2_available() noexcept {
#if KOI_NNUE_COMPILED_AVX2
    return cpu_supports_avx2();
#else
    return false;
#endif
}

void accumulate_hidden_scalar(const NnueNetwork& network,
                              std::span<const std::int8_t> encoded,
                              std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        std::int64_t activation = network.hidden_bias[hidden];
        for (std::size_t feature = 0; feature < encoded.size(); ++feature) {
            activation += static_cast<std::int64_t>(encoded[feature]) *
                network.feature_weights[feature * hidden_units + hidden];
        }
        activation = std::clamp<std::int64_t>(
            activation, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max());
        sums[hidden] = static_cast<std::int32_t>(activation);
    }
}

#if KOI_NNUE_COMPILED_AVX2
void accumulate_hidden_avx2(const NnueNetwork& network,
                            std::span<const std::int8_t> encoded,
                            std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (hidden_units % 16U != 0) {
        accumulate_hidden_scalar(network, encoded, sums);
        return;
    }
    std::size_t active_features = 0;
    for (const std::int8_t feature : encoded) {
        if (feature == 0) {
            continue;
        }
        if (feature != 1) {
            accumulate_hidden_scalar(network, encoded, sums);
            return;
        }
        ++active_features;
    }
    const std::int64_t minimum_delta = static_cast<std::int64_t>(active_features) *
        std::numeric_limits<std::int16_t>::min();
    const std::int64_t maximum_delta = static_cast<std::int64_t>(active_features) *
        std::numeric_limits<std::int16_t>::max();
    for (const std::int32_t bias : network.hidden_bias) {
        const std::int64_t minimum = static_cast<std::int64_t>(bias) + minimum_delta;
        const std::int64_t maximum = static_cast<std::int64_t>(bias) + maximum_delta;
        if (minimum < std::numeric_limits<std::int32_t>::min() ||
            maximum > std::numeric_limits<std::int32_t>::max()) {
            accumulate_hidden_scalar(network, encoded, sums);
            return;
        }
    }
    for (std::size_t hidden = 0; hidden < hidden_units; hidden += 16U) {
        __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden));
        __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden + 8U));
        for (std::size_t feature = 0; feature < encoded.size(); ++feature) {
            if (encoded[feature] == 0) {
                continue;
            }
            const auto* feature_weights = network.feature_weights.data() +
                feature * hidden_units + hidden;
            const __m256i weights = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(feature_weights));
            const __m128i low_weights = _mm256_castsi256_si128(weights);
            const __m128i high_weights = _mm256_extracti128_si256(weights, 1);
            low = _mm256_add_epi32(low, _mm256_cvtepi16_epi32(low_weights));
            high = _mm256_add_epi32(high, _mm256_cvtepi16_epi32(high_weights));
        }
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden), low);
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(sums.data() + hidden + 8U), high);
    }
}
#endif

[[nodiscard]] int infer_network(const NnueNetwork& network,
                                std::span<const std::int8_t> encoded,
                                NnueAccumulator& accumulator,
                                const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    if (encoded.size() != network.manifest.layer_sizes[0] || !arrays_match_manifest(network)) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }

    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(bottleneck_units, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_avx2(network, encoded, hidden_sums);
    } else {
        accumulate_hidden_scalar(network, encoded, hidden_sums);
    }
#else
    (void)use_avx2;
    accumulate_hidden_scalar(network, encoded, hidden_sums);
#endif

    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        accumulator.values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }

    std::int64_t output = network.output_bias;
    for (std::size_t bottleneck = 0; bottleneck < bottleneck_units; ++bottleneck) {
        std::int64_t activation = network.bottleneck_bias[bottleneck];
        for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
            activation += static_cast<std::int64_t>(accumulator.values[hidden]) *
                network.bottleneck_weights[hidden * bottleneck_units + bottleneck];
        }
        activation = std::clamp<std::int64_t>(activation, 0, kKoiNnueClippedReluMaximum);
        accumulator.bottleneck_values[bottleneck] = static_cast<std::int16_t>(activation);
        output += activation * network.output_weights[bottleneck];
    }
    return clamp_score(output);
}

} // namespace

NnueNetwork NnueNetwork::synthetic() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueFormatVersion;
    network.manifest.layer_sizes = kKoiNnueLayerSizes;
    network.manifest.feature_set = std::string(kKoiNnueFeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    const std::size_t input_units = kKoiNnueLayerSizes[0];
    const std::size_t hidden_units = kKoiNnueLayerSizes[1];
    const std::size_t bottleneck_units = kKoiNnueLayerSizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(hidden_units * bottleneck_units);
    network.bottleneck_bias.resize(bottleneck_units);
    network.output_weights.resize(bottleneck_units);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
        network.output_weights[index] = static_cast<std::int8_t>((index % 3U) + 1U);
    }
    return network;
}

NnueNetwork NnueNetwork::synthetic_v2() {
    NnueNetwork network = synthetic();
    network.manifest.layer_sizes = kKoiNnuePieceSquareKingPawnV2LayerSizes;
    network.manifest.feature_set = std::string(kKoiNnuePieceSquareKingPawnV2FeatureSet);
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(hidden_units * bottleneck_units);
    network.bottleneck_bias.resize(bottleneck_units);
    network.output_weights.resize(bottleneck_units);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
        network.output_weights[index] = static_cast<std::int8_t>((index % 3U) + 1U);
    }
    return network;
}

std::expected<std::vector<std::uint8_t>, NnueError> NnueLoader::serialize(
    const NnueNetwork& network) {
    if (const auto error = validate_manifest(network.manifest); error.has_value()) {
        return std::unexpected(*error);
    }
    const auto expected_payload_size = payload_size_for(network.manifest);
    if (!expected_payload_size.has_value() || !arrays_match_manifest(network)) {
        return std::unexpected(make_error(
            NnueErrorCode::invalid_dimensions, "NNUE weight arrays do not match the manifest"));
    }
    if (network.manifest.feature_set.size() > std::numeric_limits<std::uint16_t>::max() ||
        network.manifest.quantization.size() > std::numeric_limits<std::uint16_t>::max()) {
        return std::unexpected(make_error(
            NnueErrorCode::malformed_manifest, "NNUE feature-set name is too long"));
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(*expected_payload_size);
    for (const std::int16_t weight : network.feature_weights) {
        append_little_endian(payload, weight);
    }
    for (const std::int32_t bias : network.hidden_bias) {
        append_little_endian(payload, bias);
    }
    for (const std::int8_t weight : network.bottleneck_weights) {
        payload.push_back(static_cast<std::uint8_t>(weight));
    }
    for (const std::int32_t bias : network.bottleneck_bias) {
        append_little_endian(payload, bias);
    }
    for (const std::int8_t weight : network.output_weights) {
        payload.push_back(static_cast<std::uint8_t>(weight));
    }
    append_little_endian(payload, network.output_bias);
    if (payload.size() != *expected_payload_size) {
        return std::unexpected(invalid_file_size_error());
    }
    const auto checksum = sha256(payload);

    std::vector<std::uint8_t> container;
    container.reserve(kContainerHeaderBytes + network.manifest.quantization.size() +
                      network.manifest.feature_set.size() + payload.size());
    container.insert(container.end(), kKoiNnueMagic.begin(), kKoiNnueMagic.end());
    append_little_endian(container, network.manifest.version);
    for (const std::uint32_t layer_size : network.manifest.layer_sizes) {
        append_little_endian(container, layer_size);
    }
    append_little_endian(container, static_cast<std::uint16_t>(network.manifest.quantization.size()));
    append_little_endian(container, static_cast<std::uint16_t>(network.manifest.feature_set.size()));
    append_little_endian(container, static_cast<std::uint64_t>(payload.size()));
    container.insert(container.end(), checksum.begin(), checksum.end());
    container.insert(container.end(), network.manifest.quantization.begin(), network.manifest.quantization.end());
    container.insert(container.end(), network.manifest.feature_set.begin(), network.manifest.feature_set.end());
    container.insert(container.end(), payload.begin(), payload.end());
    return container;
}

std::expected<NnueNetwork, NnueError> NnueLoader::load(
    const std::span<const std::uint8_t> container) {
    if (container.empty()) {
        return std::unexpected(make_error(NnueErrorCode::empty_container, "NNUE container is empty"));
    }
    if (container.size() < kContainerHeaderBytes) {
        return std::unexpected(invalid_file_size_error());
    }

    NnueManifest manifest;
    manifest.magic.assign(reinterpret_cast<const char*>(container.data()), kKoiNnueMagic.size());
    std::size_t offset = kKoiNnueMagic.size();
    if (!read_little_endian(container, offset, manifest.version)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE manifest is truncated"));
    }
    for (std::uint32_t& layer_size : manifest.layer_sizes) {
        if (!read_little_endian(container, offset, layer_size)) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE layer manifest is truncated"));
        }
    }
    std::uint16_t quantization_size = 0;
    std::uint16_t feature_set_size = 0;
    if (!read_little_endian(container, offset, quantization_size) ||
        !read_little_endian(container, offset, feature_set_size)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE feature-set length is truncated"));
    }
    if (!read_little_endian(container, offset, manifest.payload_size) ||
        container.size() - offset < manifest.network_sha256.size()) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest, "NNUE checksum metadata is truncated"));
    }
    std::copy_n(container.begin() + static_cast<std::ptrdiff_t>(offset),
                manifest.network_sha256.size(), manifest.network_sha256.begin());
    offset += manifest.network_sha256.size();
    if (container.size() - offset < static_cast<std::size_t>(quantization_size) + feature_set_size) {
        return std::unexpected(invalid_file_size_error());
    }
    manifest.quantization.assign(
        reinterpret_cast<const char*>(container.data() + offset), quantization_size);
    offset += quantization_size;
    manifest.feature_set.assign(
        reinterpret_cast<const char*>(container.data() + offset), feature_set_size);
    offset += feature_set_size;

    if (const auto error = validate_manifest(manifest); error.has_value()) {
        return std::unexpected(*error);
    }
    const auto expected_payload_size = payload_size_for(manifest);
    if (!expected_payload_size.has_value() || manifest.payload_size != *expected_payload_size ||
        manifest.payload_size != container.size() - offset) {
        return std::unexpected(invalid_file_size_error());
    }
    const auto payload = container.subspan(offset, static_cast<std::size_t>(manifest.payload_size));
    if (sha256(payload) != manifest.network_sha256) {
        return std::unexpected(make_error(NnueErrorCode::checksum_mismatch, "NNUE payload checksum mismatch"));
    }

    NnueNetwork network;
    network.manifest = std::move(manifest);
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    const std::size_t feature_weight_count = input_units * hidden_units;
    std::size_t payload_offset = 0;
    network.feature_weights.resize(feature_weight_count);
    for (std::int16_t& weight : network.feature_weights) {
        if (!read_little_endian(payload, payload_offset, weight)) {
            return std::unexpected(invalid_file_size_error());
        }
    }
    network.hidden_bias.resize(hidden_units);
    for (std::int32_t& bias : network.hidden_bias) {
        std::uint32_t encoded = 0;
        if (!read_little_endian(payload, payload_offset, encoded)) {
            return std::unexpected(invalid_file_size_error());
        }
        bias = static_cast<std::int32_t>(encoded);
    }
    network.bottleneck_weights.resize(hidden_units * bottleneck_units);
    for (std::int8_t& weight : network.bottleneck_weights) {
        if (payload_offset >= payload.size()) {
            return std::unexpected(invalid_file_size_error());
        }
        weight = static_cast<std::int8_t>(payload[payload_offset++]);
    }
    network.bottleneck_bias.resize(bottleneck_units);
    for (std::int32_t& bias : network.bottleneck_bias) {
        std::uint32_t encoded = 0;
        if (!read_little_endian(payload, payload_offset, encoded)) {
            return std::unexpected(invalid_file_size_error());
        }
        bias = static_cast<std::int32_t>(encoded);
    }
    network.output_weights.resize(bottleneck_units);
    for (std::int8_t& weight : network.output_weights) {
        if (payload_offset >= payload.size()) {
            return std::unexpected(invalid_file_size_error());
        }
        weight = static_cast<std::int8_t>(payload[payload_offset++]);
    }
    std::uint32_t output_bias = 0;
    if (!read_little_endian(payload, payload_offset, output_bias)) {
        return std::unexpected(invalid_file_size_error());
    }
    network.output_bias = static_cast<std::int32_t>(output_bias);
    if (payload_offset != payload.size()) {
        return std::unexpected(invalid_file_size_error());
    }
    return network;
}

std::expected<NnueNetwork, NnueError> NnueLoader::load_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to open NNUE container"));
    }
    const std::streampos end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to size NNUE container"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(make_error(NnueErrorCode::io_error, "unable to read NNUE container"));
    }
    return load(bytes);
}

NnueWorker::NnueWorker(std::shared_ptr<const NnueNetwork> weights)
    : weights_(std::move(weights)) {
    if (weights_ && !validate_manifest(weights_->manifest).has_value() &&
        arrays_match_manifest(*weights_)) {
        accumulator_.values.resize(weights_->manifest.layer_sizes[1]);
        accumulator_.bottleneck_values.resize(weights_->manifest.layer_sizes[2]);
    }
}

int NnueWorker::evaluate(const EvaluationFeatures& features, const Color perspective,
                         const NnueInferencePath path) {
    if (!weights_ || validate_manifest(weights_->manifest).has_value() ||
        !arrays_match_manifest(*weights_)) {
        return 0;
    }
    const bool use_v2 = weights_->manifest.feature_set ==
        kKoiNnuePieceSquareKingPawnV2FeatureSet;
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    int score = 0;
    if (use_v2) {
        const auto encoded = EvaluationFeatureExtractor::encode_piece_square_king_pawn_v2(features);
        score = infer_network(*weights_, encoded, accumulator_, use_avx2);
    } else {
        const auto encoded = EvaluationFeatureExtractor::encode_piece_square_v1(features);
        score = infer_network(*weights_, encoded, accumulator_, use_avx2);
    }
    return perspective == Color::white ? score : -score;
}

int NnueWorker::evaluate(const GameState& state, const Color perspective,
                         const NnueInferencePath path) {
    return evaluate(EvaluationFeatureExtractor::extract(state), perspective, path);
}

namespace {

class NnueEvaluatorWorker final : public EvaluatorWorker {
public:
    explicit NnueEvaluatorWorker(std::shared_ptr<const NnueNetwork> weights)
        : worker_(std::move(weights)) {}

    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) override {
        return worker_.evaluate(state, perspective);
    }

private:
    NnueWorker worker_;
};

} // namespace

NnueEvaluator::NnueEvaluator(std::shared_ptr<const NnueNetwork> weights)
    : NnueEvaluator(std::move(weights), std::make_shared<ClassicalEvaluator>()) {}

NnueEvaluator::NnueEvaluator(std::shared_ptr<const NnueNetwork> weights,
                             std::shared_ptr<const Evaluator> fallback)
    : weights_(std::move(weights)), fallback_(std::move(fallback)) {}

int NnueEvaluator::evaluate(const GameState& state, const Color perspective) const {
    if (!weights_) {
        return fallback_ ? fallback_->evaluate(state, perspective) : 0;
    }
    NnueWorker worker(weights_);
    return worker.evaluate(state, perspective);
}

std::unique_ptr<EvaluatorWorker> NnueEvaluator::create_worker() const {
    if (!weights_) {
        return {};
    }
    return std::make_unique<NnueEvaluatorWorker>(weights_);
}

NnueWorker NnueEvaluator::make_worker() const {
    return NnueWorker(weights_);
}

EvaluatorSelection make_evaluator(std::optional<std::filesystem::path> nnue_path) {
    EvaluatorSelection selection;
    if (!nnue_path.has_value()) {
        selection.evaluator = std::make_shared<ClassicalEvaluator>();
        return selection;
    }

    const auto loaded = NnueLoader::load_file(*nnue_path);
    if (!loaded.has_value()) {
        selection.evaluator = std::make_shared<ClassicalEvaluator>();
        selection.nnue_error = loaded.error();
        return selection;
    }
    selection.evaluator = std::make_shared<NnueEvaluator>(
        std::make_shared<const NnueNetwork>(std::move(*loaded)));
    selection.nnue_enabled = true;
    return selection;
}

} // namespace koi
