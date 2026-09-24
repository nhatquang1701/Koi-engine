#include "koi/nnue.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "koi/gpu/gpu_nnue_service.hpp"
#include "koi/gpu/nnue_gpu_evaluator.hpp"

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

// The largest supported container is a v5 payload of 36864 inputs x 1536
// hidden int16 weights (about 113 MiB); 256 MiB leaves room for a future
// wider hidden layer while keeping a corrupt, truncated, or hostile EvalFile
// from requesting an unbounded allocation before any validation runs.
constexpr std::uintmax_t kMaximumNetworkBytes = 256ull * 1024ull * 1024ull;

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

[[nodiscard]] constexpr bool is_halfka_king_bucket_v1(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaKingBucketV1FormatVersion;
}

[[nodiscard]] constexpr bool is_halfka_threat_v5(
    const NnueManifest& manifest) noexcept {
    return manifest.version == kKoiNnueHalfkaThreatV5FormatVersion;
}

[[nodiscard]] std::optional<std::size_t> payload_size_for(const NnueManifest& manifest) noexcept {
    if (is_halfka_threat_v5(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaThreatV5FeatureSet) {
            return std::nullopt;
        }
        const std::size_t input_units = manifest.layer_sizes[0];
        const std::size_t hidden_units = manifest.layer_sizes[1];
        const std::size_t output_buckets = manifest.layer_sizes[2];
        const std::size_t l1_units = manifest.layer_sizes[3];
        return input_units * hidden_units * sizeof(std::int16_t) +
            hidden_units * sizeof(std::int32_t) +
            l1_units * hidden_units * sizeof(std::int8_t) +
            l1_units * sizeof(std::int32_t) +
            output_buckets * l1_units * sizeof(std::int8_t) +
            output_buckets * sizeof(std::int32_t);
    }
    if (is_halfka_king_bucket_v1(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaKingBucketV1FeatureSet) {
            return std::nullopt;
        }
        const std::size_t input_units = manifest.layer_sizes[0];
        const std::size_t hidden_units = manifest.layer_sizes[1];
        const std::size_t output_buckets = manifest.layer_sizes[2];
        return input_units * hidden_units * sizeof(std::int16_t) +
            hidden_units * sizeof(std::int32_t) +
            output_buckets * (hidden_units / 2U) * sizeof(std::int8_t) +
            output_buckets * sizeof(std::int32_t);
    }
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
    if (manifest.version != kKoiNnueFormatVersion &&
        manifest.version != kKoiNnuePerspectiveV3FormatVersion &&
        manifest.version != kKoiNnueHalfkaKingBucketV1FormatVersion &&
        manifest.version != kKoiNnueHalfkaThreatV5FormatVersion) {
        return make_error(NnueErrorCode::unsupported_version, "unsupported Koi NNUE container version");
    }
    if (is_halfka_threat_v5(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaThreatV5FeatureSet) {
            return make_error(NnueErrorCode::unsupported_feature_set,
                              "NNUE v5 requires the halfka-king-bucket-v1+threat-pairs-v1 feature set");
        }
        const std::uint32_t hidden_units = manifest.layer_sizes[1];
        const std::uint32_t l1_units = manifest.layer_sizes[3];
        if (manifest.layer_sizes[0] != kKoiNnueHalfkaThreatV5FeatureCount ||
            manifest.layer_sizes[2] != kKoiNnueOutputBucketCount ||
            hidden_units < kKoiNnueMinimumHiddenUnits ||
            hidden_units > kKoiNnueMaximumHiddenUnits || hidden_units % 2U != 0 ||
            l1_units < kKoiNnueMinimumL1Units || l1_units > kKoiNnueMaximumL1Units) {
            return make_error(
                NnueErrorCode::invalid_dimensions,
                "NNUE v5 layer manifest must be 36864 inputs, even hidden units in [32, 8192], "
                "8 output buckets, and 8..128 L1 units");
        }
        if (manifest.quantization != kKoiNnueQuantization) {
            return make_error(NnueErrorCode::unsupported_quantization,
                              "only int16/int8 Koi NNUE quantization is supported");
        }
        return std::nullopt;
    }
    if (is_halfka_king_bucket_v1(manifest)) {
        if (manifest.feature_set != kKoiNnueHalfkaKingBucketV1FeatureSet) {
            return make_error(NnueErrorCode::unsupported_feature_set,
                              "NNUE v4 requires the halfka-king-bucket-v1 feature set");
        }
        const std::uint32_t hidden_units = manifest.layer_sizes[1];
        if (manifest.layer_sizes[0] != kKoiNnueHalfkaKingBucketV1FeatureCount ||
            manifest.layer_sizes[2] != kKoiNnueOutputBucketCount ||
            manifest.layer_sizes[3] != 0 ||
            hidden_units < kKoiNnueMinimumHiddenUnits ||
            hidden_units > kKoiNnueMaximumHiddenUnits || hidden_units % 2U != 0) {
            return make_error(
                NnueErrorCode::invalid_dimensions,
                "NNUE v4 layer manifest must be 9216 inputs, even hidden units in [32, 8192], "
                "8 output buckets, and a zero reserved word");
        }
        if (manifest.quantization != kKoiNnueQuantization) {
            return make_error(NnueErrorCode::unsupported_quantization,
                              "only int16/int8 Koi NNUE quantization is supported");
        }
        return std::nullopt;
    }
    if (manifest.version == kKoiNnuePerspectiveV3FormatVersion &&
        manifest.feature_set != kKoiNnuePieceSquareKingPawnV2FeatureSet) {
        return make_error(NnueErrorCode::unsupported_feature_set,
                          "NNUE v3 requires the piece-square-king-pawn-v2 feature set");
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
    if (is_halfka_threat_v5(network.manifest)) {
        return network.feature_weights.size() ==
                static_cast<std::size_t>(sizes[0]) * sizes[1] &&
            network.hidden_bias.size() == sizes[1] &&
            network.l1_weights.size() == static_cast<std::size_t>(sizes[3]) * sizes[1] &&
            network.l1_bias.size() == sizes[3] &&
            network.bottleneck_weights.size() == static_cast<std::size_t>(sizes[2]) * sizes[3] &&
            network.bottleneck_bias.size() == sizes[2] &&
            network.output_weights.empty();
    }
    if (is_halfka_king_bucket_v1(network.manifest)) {
        return network.feature_weights.size() ==
                static_cast<std::size_t>(sizes[0]) * sizes[1] &&
            network.hidden_bias.size() == sizes[1] &&
            network.bottleneck_weights.size() ==
                static_cast<std::size_t>(sizes[2]) * (sizes[1] / 2U) &&
            network.bottleneck_bias.size() == sizes[2] &&
            network.output_weights.empty();
    }
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

void accumulate_hidden_sparse_scalar(const NnueNetwork& network,
                                     std::span<const std::uint16_t> active,
                                     std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        std::int64_t activation = network.hidden_bias[hidden];
        for (const std::uint16_t feature : active) {
            activation += network.feature_weights[
                static_cast<std::size_t>(feature) * hidden_units + hidden];
        }
        activation = std::clamp<std::int64_t>(
            activation, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max());
        sums[hidden] = static_cast<std::int32_t>(activation);
    }
}

#if KOI_NNUE_COMPILED_AVX2
void accumulate_hidden_sparse_avx2(const NnueNetwork& network,
                                   std::span<const std::uint16_t> active,
                                   std::vector<std::int32_t>& sums) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (hidden_units % 16U != 0) {
        accumulate_hidden_sparse_scalar(network, active, sums);
        return;
    }
    // The active features are binary inputs, so the first layer is a sum of
    // weight rows.  Bounding the accumulated delta keeps the int32 arithmetic
    // in range exactly like the dense path; the active list is tiny (tens of
    // entries), so this is safe for any realistic network here.
    const std::int64_t minimum_delta = static_cast<std::int64_t>(active.size()) *
        std::numeric_limits<std::int16_t>::min();
    const std::int64_t maximum_delta = static_cast<std::int64_t>(active.size()) *
        std::numeric_limits<std::int16_t>::max();
    for (const std::int32_t bias : network.hidden_bias) {
        if (static_cast<std::int64_t>(bias) + minimum_delta <
                std::numeric_limits<std::int32_t>::min() ||
            static_cast<std::int64_t>(bias) + maximum_delta >
                std::numeric_limits<std::int32_t>::max()) {
            accumulate_hidden_sparse_scalar(network, active, sums);
            return;
        }
    }
    for (std::size_t hidden = 0; hidden < hidden_units; hidden += 16U) {
        __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden));
        __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(network.hidden_bias.data() + hidden + 8U));
        for (const std::uint16_t feature : active) {
            const auto* feature_weights = network.feature_weights.data() +
                static_cast<std::size_t>(feature) * hidden_units + hidden;
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

[[nodiscard]] int complete_inference(const NnueNetwork& network,
                                     std::span<const std::int32_t> hidden_sums,
                                     NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
        accumulator.values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }

    std::int64_t output = network.output_bias;
    for (std::size_t bottleneck = 0; bottleneck < bottleneck_units; ++bottleneck) {
        std::int64_t activation = 0;
        for (std::size_t hidden = 0; hidden < hidden_units; ++hidden) {
            activation += static_cast<std::int64_t>(accumulator.values[hidden]) *
                network.bottleneck_weights[hidden * bottleneck_units + bottleneck];
        }
        if (network.bottleneck_shift > 0) {
            activation >>= network.bottleneck_shift;
        }
        activation += network.bottleneck_bias[bottleneck];
        activation = std::clamp<std::int64_t>(activation, 0, kKoiNnueClippedReluMaximum);
        accumulator.bottleneck_values[bottleneck] = static_cast<std::int16_t>(activation);
        output += activation * network.output_weights[bottleneck];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

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
    return complete_inference(network, hidden_sums, accumulator);
}

[[nodiscard]] int infer_network_sparse(const NnueNetwork& network,
                                       std::span<const std::uint16_t> active,
                                       NnueAccumulator& accumulator,
                                       const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t bottleneck_units = network.manifest.layer_sizes[2];
    if (!arrays_match_manifest(network)) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }

    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(bottleneck_units, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, active, hidden_sums);
    } else {
        accumulate_hidden_sparse_scalar(network, active, hidden_sums);
    }
#else
    (void)use_avx2;
    accumulate_hidden_sparse_scalar(network, active, hidden_sums);
#endif
    return complete_inference(network, hidden_sums, accumulator);
}

// v4 inference: the hidden layer is still a sum of binary feature rows, then
// the clipped activations feed pair products p[j] = a[j] * a[j + hidden/2]
// and one linear head selected by the piece-count bucket.
[[nodiscard]] std::size_t output_bucket_for_pieces(const std::size_t pieces) noexcept {
    const std::size_t missing = 32U - std::min<std::size_t>(32U, pieces);
    return std::min<std::size_t>(7U, missing / 4U);
}

[[nodiscard]] std::size_t piece_count_of(const EvaluationFeatures& features) noexcept {
    std::size_t pieces = 0;
    for (const Piece& piece : features.position.board) {
        if (!piece.empty()) {
            ++pieces;
        }
    }
    return pieces;
}

[[nodiscard]] std::size_t piece_count_bucket(const EvaluationFeatures& features) noexcept {
    return output_bucket_for_pieces(piece_count_of(features));
}

[[nodiscard]] constexpr PieceType promotion_piece_type(const Promotion promotion) noexcept {
    switch (promotion) {
        case Promotion::knight: return PieceType::knight;
        case Promotion::bishop: return PieceType::bishop;
        case Promotion::rook: return PieceType::rook;
        case Promotion::queen: return PieceType::queen;
        default: return PieceType::pawn;
    }
}

void fill_v4_activations(std::span<const std::int32_t> hidden_sums,
                         NnueAccumulator& accumulator) noexcept {
    for (std::size_t hidden = 0; hidden < hidden_sums.size(); ++hidden) {
        accumulator.values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

[[nodiscard]] int finish_inference_v4_scalar(const NnueNetwork& network,
                                             std::span<const std::int32_t> hidden_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t pair_count = network.manifest.layer_sizes[1] / 2U;
    fill_v4_activations(hidden_sums, accumulator);
    const auto* weights = network.bottleneck_weights.data() + bucket * pair_count;
    std::int64_t output = network.bottleneck_bias[bucket];
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
        const std::int16_t pair_product = static_cast<std::int16_t>(
            accumulator.values[pair] * accumulator.values[pair + pair_count]);
        accumulator.bottleneck_values[pair] = pair_product;
        output += static_cast<std::int64_t>(weights[pair]) * pair_product;
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

#if KOI_NNUE_COMPILED_AVX2
void compute_pair_products_avx2(std::span<const std::int16_t> activations,
                                std::span<std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i low = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(activations.data() + pair));
        const __m256i high = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(activations.data() + pair + pair_count));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(pairs.data() + pair),
                            _mm256_mullo_epi16(low, high));
    }
    for (; pair < pair_count; ++pair) {
        pairs[pair] = static_cast<std::int16_t>(
            activations[pair] * activations[pair + pair_count]);
    }
}

[[nodiscard]] std::int64_t bucket_dot_avx2(const NnueNetwork& network,
                                           const std::size_t bucket,
                                           std::span<const std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    const auto* weights = network.bottleneck_weights.data() + bucket * pair_count;
    // Every int32 lane accumulates at most pair_count products of a weight
    // bounded by 127 and a pair product bounded by 127 * 127.  Fall back to the
    // scalar int64 sum when that worst case cannot fit, so the vector path is
    // always equivalent to the scalar reference.
    constexpr std::int64_t kMaximumPairProduct = 127 * 127;
    if (static_cast<std::int64_t>(pair_count) * 127 * kMaximumPairProduct >
        std::numeric_limits<std::int32_t>::max()) {
        std::int64_t output = 0;
        for (std::size_t pair = 0; pair < pair_count; ++pair) {
            output += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
        }
        return output;
    }
    __m256i accumulator = _mm256_setzero_si256();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i pair_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(pairs.data() + pair));
        const __m128i weight_bytes = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(weights + pair));
        const __m256i weight_values = _mm256_cvtepi8_epi16(weight_bytes);
        accumulator = _mm256_add_epi32(
            accumulator, _mm256_madd_epi16(pair_values, weight_values));
    }
    alignas(32) std::array<std::int32_t, 8> lanes{};
    _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), accumulator);
    std::int64_t output = 0;
    for (const std::int32_t lane : lanes) {
        output += lane;
    }
    for (; pair < pair_count; ++pair) {
        output += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
    }
    return output;
}

[[nodiscard]] int finish_inference_v4_avx2(const NnueNetwork& network,
                                           std::span<const std::int32_t> hidden_sums,
                                           const std::size_t bucket,
                                           NnueAccumulator& accumulator) noexcept {
    fill_v4_activations(hidden_sums, accumulator);
    compute_pair_products_avx2(accumulator.values, accumulator.bottleneck_values);
    std::int64_t output = network.bottleneck_bias[bucket] +
        bucket_dot_avx2(network, bucket, accumulator.bottleneck_values);
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}
#endif

[[nodiscard]] int infer_network_sparse_v4(const NnueNetwork& network,
                                          std::span<const std::uint16_t> active,
                                          const std::size_t bucket,
                                          NnueAccumulator& accumulator,
                                          const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(hidden_units / 2U, 0);
    std::vector<std::int32_t> hidden_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, active, hidden_sums);
        return finish_inference_v4_avx2(network, hidden_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
    accumulate_hidden_sparse_scalar(network, active, hidden_sums);
    return finish_inference_v4_scalar(network, hidden_sums, bucket, accumulator);
}

// v5 inference: both perspective accumulators feed cross-perspective pair
// products p[j] = a_own[j] * a_opp[j], then one CReLU hidden layer and a
// linear head selected by the piece-count bucket.  The scalar path is the
// correctness boundary; the AVX2 helpers below must agree with it exactly.
void fill_v5_activations(std::span<const std::int32_t> hidden_sums,
                         std::span<std::int16_t> values) noexcept {
    for (std::size_t hidden = 0; hidden < hidden_sums.size(); ++hidden) {
        values[hidden] = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            hidden_sums[hidden], 0, kKoiNnueClippedReluMaximum));
    }
}

void compute_cross_pair_products(std::span<const std::int16_t> own,
                                 std::span<const std::int16_t> opp,
                                 std::span<std::int16_t> pairs) noexcept {
    for (std::size_t pair = 0; pair < pairs.size(); ++pair) {
        pairs[pair] = static_cast<std::int16_t>(own[pair] * opp[pair]);
    }
}

void l1_activations_scalar(const NnueNetwork& network,
                           std::span<const std::int16_t> pairs,
                           std::span<std::int32_t> activations) noexcept {
    const std::size_t pair_count = pairs.size();
    for (std::size_t unit = 0; unit < activations.size(); ++unit) {
        const std::int8_t* weights = network.l1_weights.data() + unit * pair_count;
        std::int64_t sum = network.l1_bias[unit];
        for (std::size_t pair = 0; pair < pair_count; ++pair) {
            sum += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
        }
        if (network.l1_shift > 0) {
            sum >>= network.l1_shift;
        }
        activations[unit] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            sum, 0, kKoiNnueClippedReluMaximum));
    }
}

[[nodiscard]] int finish_inference_v5_scalar(const NnueNetwork& network,
                                             std::span<const std::int32_t> own_sums,
                                             std::span<const std::int32_t> opp_sums,
                                             const std::size_t bucket,
                                             NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units;
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(pair_count, 0);
    std::vector<std::int16_t> opp_values(hidden_units, 0);
    fill_v5_activations(own_sums, accumulator.values);
    fill_v5_activations(opp_sums, opp_values);
    compute_cross_pair_products(accumulator.values, opp_values,
                                accumulator.bottleneck_values);
    std::vector<std::int32_t> l1(l1_units, 0);
    l1_activations_scalar(network, accumulator.bottleneck_values, l1);
    std::int64_t output = network.bottleneck_bias[bucket];
    const std::int8_t* head = network.bottleneck_weights.data() + bucket * l1_units;
    for (std::size_t unit = 0; unit < l1_units; ++unit) {
        output += static_cast<std::int64_t>(head[unit]) * l1[unit];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}

#if KOI_NNUE_COMPILED_AVX2
void compute_cross_pair_products_avx2(std::span<const std::int16_t> own,
                                      std::span<const std::int16_t> opp,
                                      std::span<std::int16_t> pairs) noexcept {
    const std::size_t pair_count = pairs.size();
    std::size_t pair = 0;
    for (; pair + 16U <= pair_count; pair += 16U) {
        const __m256i own_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(own.data() + pair));
        const __m256i opp_values = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(opp.data() + pair));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(pairs.data() + pair),
                            _mm256_mullo_epi16(own_values, opp_values));
    }
    for (; pair < pair_count; ++pair) {
        pairs[pair] = static_cast<std::int16_t>(own[pair] * opp[pair]);
    }
}

void l1_activations_avx2(const NnueNetwork& network,
                         std::span<const std::int16_t> pairs,
                         std::span<std::int32_t> activations) noexcept {
    const std::size_t pair_count = pairs.size();
    // Each int32 lane accumulates at most (pair_count / 16 + 1) * 2 products
    // bounded by 127 * 127 * 127.  Fall back to the int64 scalar path when
    // that worst case cannot fit, so the vector path is always equivalent.
    const std::int64_t lane_worst_case =
        static_cast<std::int64_t>(pair_count / 16U + 1U) * 2 * 127 * (127 * 127);
    if (lane_worst_case > std::numeric_limits<std::int32_t>::max()) {
        l1_activations_scalar(network, pairs, activations);
        return;
    }
    alignas(32) std::array<std::int32_t, 8> lanes{};
    for (std::size_t unit = 0; unit < activations.size(); ++unit) {
        const std::int8_t* weights = network.l1_weights.data() + unit * pair_count;
        __m256i accumulator = _mm256_setzero_si256();
        std::size_t pair = 0;
        for (; pair + 16U <= pair_count; pair += 16U) {
            const __m256i pair_values = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(pairs.data() + pair));
            const __m128i weight_bytes = _mm_loadu_si128(
                reinterpret_cast<const __m128i*>(weights + pair));
            const __m256i weight_values = _mm256_cvtepi8_epi16(weight_bytes);
            accumulator = _mm256_add_epi32(
                accumulator, _mm256_madd_epi16(pair_values, weight_values));
        }
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes.data()), accumulator);
        std::int64_t sum = network.l1_bias[unit];
        for (const std::int32_t lane : lanes) {
            sum += lane;
        }
        for (; pair < pair_count; ++pair) {
            sum += static_cast<std::int64_t>(weights[pair]) * pairs[pair];
        }
        if (network.l1_shift > 0) {
            sum >>= network.l1_shift;
        }
        activations[unit] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            sum, 0, kKoiNnueClippedReluMaximum));
    }
}

[[nodiscard]] int finish_inference_v5_avx2(const NnueNetwork& network,
                                           std::span<const std::int32_t> own_sums,
                                           std::span<const std::int32_t> opp_sums,
                                           const std::size_t bucket,
                                           NnueAccumulator& accumulator) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t pair_count = hidden_units;
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    accumulator.values.assign(hidden_units, 0);
    accumulator.bottleneck_values.assign(pair_count, 0);
    std::vector<std::int16_t> opp_values(hidden_units, 0);
    fill_v5_activations(own_sums, accumulator.values);
    fill_v5_activations(opp_sums, opp_values);
    compute_cross_pair_products_avx2(accumulator.values, opp_values,
                                     accumulator.bottleneck_values);
    std::vector<std::int32_t> l1(l1_units, 0);
    l1_activations_avx2(network, accumulator.bottleneck_values, l1);
    std::int64_t output = network.bottleneck_bias[bucket];
    const std::int8_t* head = network.bottleneck_weights.data() + bucket * l1_units;
    for (std::size_t unit = 0; unit < l1_units; ++unit) {
        output += static_cast<std::int64_t>(head[unit]) * l1[unit];
    }
    if (network.output_shift > 0) {
        output >>= network.output_shift;
    }
    return clamp_score(output);
}
#endif

[[nodiscard]] int infer_network_sparse_v5(const NnueNetwork& network,
                                          std::span<const std::uint16_t> own_active,
                                          std::span<const std::uint16_t> opp_active,
                                          const std::size_t bucket,
                                          NnueAccumulator& accumulator,
                                          const bool use_avx2) noexcept {
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    if (!arrays_match_manifest(network) || bucket >= network.manifest.layer_sizes[2]) {
        accumulator.values.clear();
        accumulator.bottleneck_values.clear();
        return 0;
    }
    std::vector<std::int32_t> own_sums(hidden_units, 0);
    std::vector<std::int32_t> opp_sums(hidden_units, 0);
#if KOI_NNUE_COMPILED_AVX2
    if (use_avx2) {
        accumulate_hidden_sparse_avx2(network, own_active, own_sums);
        accumulate_hidden_sparse_avx2(network, opp_active, opp_sums);
        return finish_inference_v5_avx2(network, own_sums, opp_sums, bucket, accumulator);
    }
#else
    (void)use_avx2;
#endif
    accumulate_hidden_sparse_scalar(network, own_active, own_sums);
    accumulate_hidden_sparse_scalar(network, opp_active, opp_sums);
    return finish_inference_v5_scalar(network, own_sums, opp_sums, bucket, accumulator);
}

#if KOI_NNUE_COMPILED_AVX2
// Applies one feature row to a perspective accumulator with 8 int32 lanes.
// The scalar path accumulates in int64 and clamps to int32; the vector path
// uses int32 lanes and redoes an 8-lane block scalar only when the addition
// would have overflowed (same-sign operands with a changed result sign).
void apply_delta_avx2(std::int32_t* values, const std::int16_t* weights,
                      const std::size_t count, const int sign) noexcept {
    const __m256i zero = _mm256_setzero_si256();
    std::size_t index = 0;
    for (; index + 8U <= count; index += 8U) {
        const __m256i current =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(values + index));
        __m256i delta = _mm256_cvtepi16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(weights + index)));
        if (sign < 0) {
            delta = _mm256_sub_epi32(zero, delta);
        }
        const __m256i sum = _mm256_add_epi32(current, delta);
        const __m256i differing_operands =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(current, delta));
        const __m256i differing_result =
            _mm256_cmpgt_epi32(zero, _mm256_xor_si256(current, sum));
        const __m256i overflow = _mm256_andnot_si256(differing_operands, differing_result);
        if (_mm256_movemask_epi8(overflow) != 0) {
            for (std::size_t lane = index; lane < index + 8U; ++lane) {
                const std::int64_t updated = static_cast<std::int64_t>(values[lane]) +
                    static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[lane]);
                values[lane] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    updated, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
            continue;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(values + index), sum);
    }
    for (; index < count; ++index) {
        const std::int64_t updated = static_cast<std::int64_t>(values[index]) +
            static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[index]);
        values[index] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            updated, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    }
}
#endif

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

NnueNetwork NnueNetwork::synthetic_v4() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueHalfkaKingBucketV1FormatVersion;
    network.manifest.layer_sizes = {
        kKoiNnueHalfkaKingBucketV1FeatureCount, 32, kKoiNnueOutputBucketCount, 0};
    network.manifest.feature_set = std::string(kKoiNnueHalfkaKingBucketV1FeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    network.hidden_shift = 7;
    network.output_shift = 15;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.bottleneck_weights.resize(output_buckets * (hidden_units / 2U));
    network.bottleneck_bias.resize(output_buckets);
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
    }
    return network;
}

NnueNetwork NnueNetwork::synthetic_v5() {
    NnueNetwork network;
    network.manifest.magic = std::string(kKoiNnueMagic);
    network.manifest.version = kKoiNnueHalfkaThreatV5FormatVersion;
    network.manifest.layer_sizes = {
        kKoiNnueHalfkaThreatV5FeatureCount, 32, kKoiNnueOutputBucketCount, 8};
    network.manifest.feature_set = std::string(kKoiNnueHalfkaThreatV5FeatureSet);
    network.manifest.quantization = std::string(kKoiNnueQuantization);
    network.hidden_shift = 7;
    network.l1_shift = 6;
    network.output_shift = 12;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_buckets = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    network.feature_weights.resize(input_units * hidden_units);
    network.hidden_bias.resize(hidden_units);
    network.l1_weights.resize(l1_units * hidden_units);
    network.l1_bias.resize(l1_units);
    network.bottleneck_weights.resize(output_buckets * l1_units);
    network.bottleneck_bias.resize(output_buckets);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] = static_cast<std::int16_t>(static_cast<int>(index % 5U) - 2);
    }
    for (std::size_t index = 0; index < network.hidden_bias.size(); ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.l1_weights.size(); ++index) {
        network.l1_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.l1_bias.size(); ++index) {
        network.l1_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] = static_cast<std::int8_t>(static_cast<int>(index % 3U) - 1);
    }
    for (std::size_t index = 0; index < network.bottleneck_bias.size(); ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index) - 1;
    }
    return network;
}

std::expected<std::vector<std::uint8_t>, NnueError> NnueLoader::serialize(
    const NnueNetwork& network) {
    if (const auto error = validate_manifest(network.manifest); error.has_value()) {
        return std::unexpected(*error);
    }
    if (network.manifest.version < kKoiNnuePerspectiveV3FormatVersion &&
        (network.hidden_shift != 0 || network.bottleneck_shift != 0 ||
         network.output_shift != 0)) {
        return std::unexpected(make_error(
            NnueErrorCode::malformed_manifest,
            "NNUE v1/v2 containers cannot carry nonzero shifts"));
    }
    if (is_halfka_threat_v5(network.manifest)) {
        if (network.hidden_shift > kKoiNnueMaximumShift ||
            network.output_shift > kKoiNnueMaximumShift ||
            network.l1_shift > kKoiNnueMaximumShift ||
            network.bottleneck_shift != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                              "NNUE v5 shift is out of range"));
        }
    } else if (is_halfka_king_bucket_v1(network.manifest)) {
        if (network.hidden_shift > kKoiNnueMaximumShift ||
            network.output_shift > kKoiNnueMaximumShift ||
            network.bottleneck_shift != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                              "NNUE v4 shift is out of range"));
        }
    } else if (network.manifest.version >= kKoiNnuePerspectiveV3FormatVersion &&
               (network.hidden_shift > kKoiNnueMaximumShift ||
                network.bottleneck_shift > kKoiNnueMaximumShift ||
                network.output_shift > kKoiNnueMaximumShift)) {
        return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                          "NNUE v3 shift is out of range"));
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
    if (is_halfka_threat_v5(network.manifest)) {
        for (const std::int8_t weight : network.l1_weights) {
            payload.push_back(static_cast<std::uint8_t>(weight));
        }
        for (const std::int32_t bias : network.l1_bias) {
            append_little_endian(payload, bias);
        }
    }
    for (const std::int8_t weight : network.bottleneck_weights) {
        payload.push_back(static_cast<std::uint8_t>(weight));
    }
    for (const std::int32_t bias : network.bottleneck_bias) {
        append_little_endian(payload, bias);
    }
    if (!is_halfka_king_bucket_v1(network.manifest) &&
        !is_halfka_threat_v5(network.manifest)) {
        for (const std::int8_t weight : network.output_weights) {
            payload.push_back(static_cast<std::uint8_t>(weight));
        }
        append_little_endian(payload, network.output_bias);
    }
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
    if (is_halfka_threat_v5(network.manifest)) {
        container.push_back(network.hidden_shift);
        container.push_back(network.output_shift);
        container.push_back(network.l1_shift);
        container.push_back(0);
    } else if (is_halfka_king_bucket_v1(network.manifest)) {
        container.push_back(network.hidden_shift);
        container.push_back(network.output_shift);
        container.push_back(0);
        container.push_back(0);
    } else if (network.manifest.version >= kKoiNnuePerspectiveV3FormatVersion) {
        container.push_back(network.hidden_shift);
        container.push_back(network.bottleneck_shift);
        container.push_back(network.output_shift);
        container.push_back(0);
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
    std::uint8_t hidden_shift = 0;
    std::uint8_t bottleneck_shift = 0;
    std::uint8_t output_shift = 0;
    std::uint8_t l1_shift = 0;
    if (manifest.version == kKoiNnueHalfkaThreatV5FormatVersion) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        output_shift = container[offset++];
        l1_shift = container[offset++];
        const std::uint8_t reserved = container[offset++];
        if (reserved != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5 reserved header byte must be zero"));
        }
        if (hidden_shift > kKoiNnueMaximumShift || output_shift > kKoiNnueMaximumShift ||
            l1_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v5 shift is out of range"));
        }
    } else if (manifest.version == kKoiNnueHalfkaKingBucketV1FormatVersion) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        output_shift = container[offset++];
        const std::uint8_t reserved_low = container[offset++];
        const std::uint8_t reserved_high = container[offset++];
        if (reserved_low != 0 || reserved_high != 0) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 reserved header bytes must be zero"));
        }
        if (hidden_shift > kKoiNnueMaximumShift || output_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v4 shift is out of range"));
        }
    } else if (manifest.version >= kKoiNnuePerspectiveV3FormatVersion) {
        if (container.size() - offset < 4) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v3 scale metadata is truncated"));
        }
        hidden_shift = container[offset++];
        bottleneck_shift = container[offset++];
        output_shift = container[offset++];
        ++offset;
        if (hidden_shift > kKoiNnueMaximumShift ||
            bottleneck_shift > kKoiNnueMaximumShift ||
            output_shift > kKoiNnueMaximumShift) {
            return std::unexpected(make_error(NnueErrorCode::malformed_manifest,
                                               "NNUE v3 shift is out of range"));
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
    network.hidden_shift = hidden_shift;
    network.bottleneck_shift = bottleneck_shift;
    network.output_shift = output_shift;
    network.l1_shift = l1_shift;
    const std::size_t input_units = network.manifest.layer_sizes[0];
    const std::size_t hidden_units = network.manifest.layer_sizes[1];
    const std::size_t output_units = network.manifest.layer_sizes[2];
    const std::size_t l1_units = network.manifest.layer_sizes[3];
    const bool threat_v5 = is_halfka_threat_v5(network.manifest);
    const bool halfka_v4 = is_halfka_king_bucket_v1(network.manifest);
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
    if (threat_v5) {
        network.l1_weights.resize(l1_units * hidden_units);
        for (std::int8_t& weight : network.l1_weights) {
            if (payload_offset >= payload.size()) {
                return std::unexpected(invalid_file_size_error());
            }
            weight = static_cast<std::int8_t>(payload[payload_offset++]);
        }
        network.l1_bias.resize(l1_units);
        for (std::int32_t& bias : network.l1_bias) {
            std::uint32_t encoded = 0;
            if (!read_little_endian(payload, payload_offset, encoded)) {
                return std::unexpected(invalid_file_size_error());
            }
            bias = static_cast<std::int32_t>(encoded);
        }
    }
    network.bottleneck_weights.resize(
        threat_v5 ? output_units * l1_units :
        (halfka_v4 ? output_units * (hidden_units / 2U) : hidden_units * output_units));
    for (std::int8_t& weight : network.bottleneck_weights) {
        if (payload_offset >= payload.size()) {
            return std::unexpected(invalid_file_size_error());
        }
        weight = static_cast<std::int8_t>(payload[payload_offset++]);
    }
    network.bottleneck_bias.resize(output_units);
    for (std::int32_t& bias : network.bottleneck_bias) {
        std::uint32_t encoded = 0;
        if (!read_little_endian(payload, payload_offset, encoded)) {
            return std::unexpected(invalid_file_size_error());
        }
        bias = static_cast<std::int32_t>(encoded);
    }
    if (!halfka_v4 && !threat_v5) {
        network.output_weights.resize(output_units);
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
    }
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
    if (static_cast<std::uintmax_t>(end) > kMaximumNetworkBytes) {
        // Reject before allocating: a bad EvalFile path (or a sparse file) must
        // not be able to exhaust memory from inside the UCI loop.
        return std::unexpected(make_error(NnueErrorCode::io_error,
                                          "NNUE container exceeds the 256 MiB limit"));
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(end));
    } catch (const std::exception&) {
        // Surface allocation failure as a load error so the caller can keep the
        // current evaluator instead of terminating on an escaping bad_alloc.
        return std::unexpected(make_error(NnueErrorCode::io_error,
                                          "unable to allocate memory for the NNUE container"));
    }
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
        const std::size_t hidden_units = weights_->manifest.layer_sizes[1];
        const bool threat_v5 = is_halfka_threat_v5(weights_->manifest);
        const bool halfka_v4 = is_halfka_king_bucket_v1(weights_->manifest);
        accumulator_.values.resize(hidden_units);
        accumulator_.bottleneck_values.resize(threat_v5 ? hidden_units :
            (halfka_v4 ? hidden_units / 2U : weights_->manifest.layer_sizes[2]));
    }
}

int NnueWorker::evaluate(const EvaluationFeatures& features, const Color perspective,
                         const NnueInferencePath path) {
    if (!weights_ || validate_manifest(weights_->manifest).has_value() ||
        !arrays_match_manifest(*weights_)) {
        return 0;
    }
    const bool use_halfka = is_halfka_king_bucket_v1(weights_->manifest);
    const bool use_v5 = is_halfka_threat_v5(weights_->manifest);
    const bool use_v2 = weights_->manifest.feature_set ==
        kKoiNnuePieceSquareKingPawnV2FeatureSet;
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    int score = 0;
    if (use_v5) {
        // Both perspectives feed the head: own is the side to move, opponent
        // is the other side.  The sign flip below keeps the documented
        // "raw score is mover-relative" contract.
        const Color mover = features.position.side_to_move;
        const NnueSparseFeaturesV5 own =
            EvaluationFeatureExtractor::encode_sparse_v5(features, mover);
        const NnueSparseFeaturesV5 opp = EvaluationFeatureExtractor::encode_sparse_v5(
            features, opposite(mover));
        score = infer_network_sparse_v5(
            *weights_, std::span<const std::uint16_t>(own.indices.data(), own.count),
            std::span<const std::uint16_t>(opp.indices.data(), opp.count),
            piece_count_bucket(features), accumulator_, use_avx2);
    } else if (use_halfka) {
        const NnueSparseFeaturesV4 sparse =
            EvaluationFeatureExtractor::encode_sparse_v4(features);
        score = infer_network_sparse_v4(
            *weights_, std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
            piece_count_bucket(features), accumulator_, use_avx2);
    } else if (use_v2) {
        const NnueSparseFeatures sparse =
            EvaluationFeatureExtractor::encode_sparse_v2(features);
        score = infer_network_sparse(
            *weights_, std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
            accumulator_, use_avx2);
    } else {
        const auto encoded = EvaluationFeatureExtractor::encode_piece_square_v1(features);
        score = infer_network(*weights_, encoded, accumulator_, use_avx2);
    }
    // The encoder expresses features from the side to move's perspective, so
    // the raw network score is already relative to the mover. Flip it only
    // when the caller asked for the opposite perspective.
    const Color mover = features.position.side_to_move;
    return perspective == mover ? score : -score;
}

int NnueWorker::evaluate(const GameState& state, const Color perspective,
                         const NnueInferencePath path) {
    if (!supports_incremental()) {
        return evaluate(EvaluationFeatureExtractor::extract(state), perspective, path);
    }
    ensure_incremental_storage();
    const bool threat_v5 = is_halfka_threat_v5(weights_->manifest);
    const Color mover = state.side_to_move();
    const std::size_t mover_index = mover == Color::white ? 0U : 1U;
    const std::size_t opponent_index = 1U - mover_index;
    const std::uint64_t key = state.position_key();
    std::span<const std::int32_t> mover_sums;
    std::span<const std::int32_t> opponent_sums;
    std::size_t pieces = 0;
    if (slot_cursor_ >= 0 &&
        slot_valid_[static_cast<std::size_t>(slot_cursor_)] != 0 &&
        slot_keys_[static_cast<std::size_t>(slot_cursor_)] == key) {
        const std::size_t slot = static_cast<std::size_t>(slot_cursor_);
        mover_sums = std::span<const std::int32_t>(
            slot_values_[mover_index].data() + slot * hidden_units_, hidden_units_);
        opponent_sums = std::span<const std::int32_t>(
            slot_values_[opponent_index].data() + slot * hidden_units_, hidden_units_);
        pieces = slot_pieces_[slot];
    } else if (scratch_valid_ && scratch_key_ == key) {
        mover_sums = std::span<const std::int32_t>(scratch_values_[mover_index]);
        opponent_sums = std::span<const std::int32_t>(scratch_values_[opponent_index]);
        pieces = scratch_pieces_;
    } else {
        // Full rebuild: the only path that materializes the feature view.
        const EvaluationFeatures features = EvaluationFeatureExtractor::extract(state);
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            const Color real = index == 0U ? Color::white : Color::black;
            if (threat_v5) {
                const NnueSparseFeaturesV5 sparse =
                    EvaluationFeatureExtractor::encode_sparse_v5(features, real);
                accumulate_hidden_sparse_scalar(
                    *weights_,
                    std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
                    scratch_values_[index]);
                const NnueSparseFeaturesThreatV1 threats =
                    EvaluationFeatureExtractor::encode_sparse_threat_v1(features, real);
                std::copy_n(threats.indices.data(), threats.count,
                            scratch_threats_[index].data());
                scratch_threat_counts_[index] = static_cast<std::uint8_t>(threats.count);
            } else {
                const NnueSparseFeaturesV4 sparse =
                    EvaluationFeatureExtractor::encode_sparse_v4(features, real);
                accumulate_hidden_sparse_scalar(
                    *weights_,
                    std::span<const std::uint16_t>(sparse.indices.data(), sparse.count),
                    scratch_values_[index]);
            }
            scratch_buckets_[index] = static_cast<std::uint8_t>(
                EvaluationFeatureExtractor::halfka_king_bucket_for(features, real));
        }
        scratch_key_ = key;
        scratch_valid_ = true;
        scratch_pieces_ = static_cast<std::uint8_t>(piece_count_of(features));
        ++incremental_fallback_count_;
        mover_sums = std::span<const std::int32_t>(scratch_values_[mover_index]);
        opponent_sums = std::span<const std::int32_t>(scratch_values_[opponent_index]);
        pieces = scratch_pieces_;
    }
    const bool use_avx2 = path == NnueInferencePath::avx2_compatible ? avx2_available() :
        (path == NnueInferencePath::automatic && avx2_available());
    (void)use_avx2;
    const std::size_t bucket = output_bucket_for_pieces(pieces);
    int score = 0;
    if (threat_v5) {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v5_avx2(*weights_, mover_sums, opponent_sums,
                                             bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v5_scalar(*weights_, mover_sums, opponent_sums,
                                               bucket, accumulator_);
        }
    } else {
#if KOI_NNUE_COMPILED_AVX2
        if (use_avx2) {
            score = finish_inference_v4_avx2(*weights_, mover_sums, bucket, accumulator_);
        } else
#endif
        {
            score = finish_inference_v4_scalar(*weights_, mover_sums, bucket, accumulator_);
        }
    }
    return perspective == mover ? score : -score;
}

bool NnueWorker::supports_incremental() const noexcept {
    return weights_ &&
        (is_halfka_king_bucket_v1(weights_->manifest) ||
         is_halfka_threat_v5(weights_->manifest)) &&
        !validate_manifest(weights_->manifest).has_value() &&
        arrays_match_manifest(*weights_);
}

void NnueWorker::ensure_incremental_storage() {
    if (storage_ready_) {
        return;
    }
    static_assert(kThreatListCapacity ==
                      sizeof(NnueSparseFeaturesThreatV1::indices) /
                          sizeof(std::uint16_t),
                  "worker threat list capacity must match the encoder capacity");
    hidden_units_ = weights_->manifest.layer_sizes[1];
    const std::size_t slot_entries = kIncrementalSlotCount * hidden_units_;
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        slot_values_[index].assign(slot_entries, 0);
        slot_buckets_[index].assign(kIncrementalSlotCount, 0);
        scratch_values_[index].assign(hidden_units_, 0);
        slot_threats_[index].assign(kIncrementalSlotCount * kThreatListCapacity, 0);
        slot_threat_counts_[index].assign(kIncrementalSlotCount, 0);
        scratch_threats_[index].assign(kThreatListCapacity, 0);
    }
    slot_keys_.assign(kIncrementalSlotCount, 0);
    slot_valid_.assign(kIncrementalSlotCount, 0);
    slot_pieces_.assign(kIncrementalSlotCount, 0);
    storage_ready_ = true;
}

void NnueWorker::copy_slot_forward(const std::size_t from_slot,
                                   const std::size_t to_slot) {
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        std::copy_n(slot_values_[index].data() + from_slot * hidden_units_,
                    hidden_units_,
                    slot_values_[index].data() + to_slot * hidden_units_);
        slot_buckets_[index][to_slot] = slot_buckets_[index][from_slot];
        slot_threat_counts_[index][to_slot] = slot_threat_counts_[index][from_slot];
        std::copy_n(slot_threats_[index].data() + from_slot * kThreatListCapacity,
                    kThreatListCapacity,
                    slot_threats_[index].data() + to_slot * kThreatListCapacity);
    }
    slot_pieces_[to_slot] = slot_pieces_[from_slot];
}

void NnueWorker::refresh_slot_perspective(const EvaluationFeatures& features,
                                          const Color perspective,
                                          const std::size_t slot) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    std::vector<std::int32_t> sums(hidden_units_, 0);
    if (is_halfka_threat_v5(weights_->manifest)) {
        const NnueSparseFeaturesV5 sparse =
            EvaluationFeatureExtractor::encode_sparse_v5(features, perspective);
        accumulate_hidden_sparse_scalar(
            *weights_,
            std::span<const std::uint16_t>(sparse.indices.data(), sparse.count), sums);
        store_threat_list(index, slot,
                          EvaluationFeatureExtractor::encode_sparse_threat_v1(
                              features, perspective));
    } else {
        const NnueSparseFeaturesV4 sparse =
            EvaluationFeatureExtractor::encode_sparse_v4(features, perspective);
        accumulate_hidden_sparse_scalar(
            *weights_,
            std::span<const std::uint16_t>(sparse.indices.data(), sparse.count), sums);
    }
    std::copy(sums.begin(), sums.end(),
              slot_values_[index].data() + slot * hidden_units_);
    slot_buckets_[index][slot] = static_cast<std::uint8_t>(
        EvaluationFeatureExtractor::halfka_king_bucket_for(features, perspective));
    slot_pieces_[slot] = static_cast<std::uint8_t>(piece_count_of(features));
}

void NnueWorker::apply_feature_delta(const std::size_t perspective,
                                     const std::size_t slot,
                                     const std::uint16_t index, const int sign) {
    std::int32_t* values = slot_values_[perspective].data() + slot * hidden_units_;
    const std::int16_t* weights =
        weights_->feature_weights.data() +
        static_cast<std::size_t>(index) * hidden_units_;
#if KOI_NNUE_COMPILED_AVX2
    // The vector path reproduces the scalar int64-accumulate/int32-clamp
    // semantics exactly, including the per-block overflow fallback, so both
    // paths agree on every input.
    if (avx2_available() && hidden_units_ % 16U == 0U) {
        apply_delta_avx2(values, weights, hidden_units_, sign);
        return;
    }
#endif
    for (std::size_t hidden = 0; hidden < hidden_units_; ++hidden) {
        const std::int64_t updated = static_cast<std::int64_t>(values[hidden]) +
            static_cast<std::int64_t>(sign) * static_cast<std::int64_t>(weights[hidden]);
        values[hidden] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            updated, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    }
}

void NnueWorker::store_threat_list(const std::size_t perspective, const std::size_t slot,
                                   const NnueSparseFeaturesThreatV1& list) {
    std::copy_n(list.indices.data(), list.count,
                slot_threats_[perspective].data() + slot * kThreatListCapacity);
    slot_threat_counts_[perspective][slot] = static_cast<std::uint8_t>(list.count);
}

void NnueWorker::apply_threat_deltas(const EvaluationFeatures& child_features,
                                     const Color perspective, const std::size_t slot) {
    const std::size_t index = perspective == Color::white ? 0U : 1U;
    const NnueSparseFeaturesThreatV1 child_list =
        EvaluationFeatureExtractor::encode_sparse_threat_v1(child_features, perspective);
    const std::uint16_t* previous =
        slot_threats_[index].data() + slot * kThreatListCapacity;
    const std::size_t previous_count = slot_threat_counts_[index][slot];
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < previous_count && new_index < child_list.count) {
        if (previous[old_index] == child_list.indices[new_index]) {
            ++old_index;
            ++new_index;
        } else if (previous[old_index] < child_list.indices[new_index]) {
            apply_feature_delta(index, slot, previous[old_index], -1);
            ++old_index;
        } else {
            apply_feature_delta(index, slot, child_list.indices[new_index], 1);
            ++new_index;
        }
    }
    for (; old_index < previous_count; ++old_index) {
        apply_feature_delta(index, slot, previous[old_index], -1);
    }
    for (; new_index < child_list.count; ++new_index) {
        apply_feature_delta(index, slot, child_list.indices[new_index], 1);
    }
    store_threat_list(index, slot, child_list);
}

void NnueWorker::apply_move_deltas(const GameState& child,
                                   const MoveMetadata& metadata,
                                   const std::size_t slot) {
    // The child's feature view is needed when a perspective's own king
    // crosses a bucket boundary and, for v5, to diff the threat feature set.
    // Everything else is derived from the move metadata, which keeps the hot
    // make path allocation- and scan-free for v4.
    const Color mover = opposite(child.side_to_move());
    const Square from = metadata.move.from();
    const Square to = metadata.move.to();
    const MoveKind kind = metadata.kind;
    const PieceType moving = metadata.moving_piece;
    // The slot was copied forward from the parent, so its piece count is the
    // parent count.  A refresh stores the exact child count, so the decrement
    // below is skipped in that case.
    const std::uint8_t parent_pieces = slot_pieces_[slot];
    std::array<bool, kIncrementalPerspectives> refreshed{};
    for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
        const Color real = index == 0U ? Color::white : Color::black;
        if (moving == PieceType::king && mover == real) {
            // Only a bucket crossing needs a rebuild; a king step inside one
            // bucket is a plain remove/add pair and castling's rook delta is
            // applied below.  The rebuild covers the king and the castling
            // rook together.
            const std::size_t child_bucket =
                EvaluationFeatureExtractor::halfka_king_bucket_for_square(to, real);
            if (child_bucket != slot_buckets_[index][slot]) {
                refresh_slot_perspective(
                    EvaluationFeatureExtractor::extract(child), real, slot);
                refreshed[index] = true;
                continue;
            }
        }
        const std::size_t bucket = slot_buckets_[index][slot];
        apply_feature_delta(index, slot,
            EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                bucket, real, Piece{moving, mover}, from), -1);
        if (metadata.captured_piece != PieceType::none) {
            Square captured = to;
            if (kind == MoveKind::en_passant) {
                captured = mover == Color::white ?
                    Square::from_index(static_cast<std::uint8_t>(to.index() - 8U)) :
                    Square::from_index(static_cast<std::uint8_t>(to.index() + 8U));
            }
            apply_feature_delta(index, slot,
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{metadata.captured_piece, opposite(mover)},
                    captured), -1);
        }
        const PieceType placed = kind == MoveKind::promotion ?
            promotion_piece_type(metadata.move.promotion()) : moving;
        apply_feature_delta(index, slot,
            EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                bucket, real, Piece{placed, mover}, to), 1);
        if (kind == MoveKind::castling) {
            const bool king_side = mover == Color::white ? to.index() == 6U :
                                                           to.index() == 62U;
            const std::uint8_t rook_from = mover == Color::white ?
                (king_side ? 7U : 0U) : (king_side ? 63U : 56U);
            const std::uint8_t rook_to = mover == Color::white ?
                (king_side ? 5U : 3U) : (king_side ? 61U : 59U);
            apply_feature_delta(index, slot,
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{PieceType::rook, mover},
                    Square::from_index(rook_from)), -1);
            apply_feature_delta(index, slot,
                EvaluationFeatureExtractor::halfka_king_bucket_feature_index(
                    bucket, real, Piece{PieceType::rook, mover},
                    Square::from_index(rook_to)), 1);
        }
    }
    if (!refreshed[0] && !refreshed[1]) {
        slot_pieces_[slot] = static_cast<std::uint8_t>(
            parent_pieces - (metadata.captured_piece != PieceType::none ? 1U : 0U));
    }
    if (is_halfka_threat_v5(weights_->manifest)) {
        const EvaluationFeatures child_features =
            EvaluationFeatureExtractor::extract(child);
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            if (!refreshed[index]) {
                apply_threat_deltas(child_features,
                    index == 0U ? Color::white : Color::black, slot);
            }
        }
    }
}

bool NnueWorker::prepare_child_slot(const int ply, const int child_slot,
                                    const std::uint64_t parent_key) {
    const std::size_t child = static_cast<std::size_t>(child_slot);
    if (scratch_valid_ && scratch_key_ == parent_key) {
        for (std::size_t index = 0; index < kIncrementalPerspectives; ++index) {
            std::copy_n(scratch_values_[index].data(), hidden_units_,
                        slot_values_[index].data() + child * hidden_units_);
            slot_buckets_[index][child] = scratch_buckets_[index];
            slot_threat_counts_[index][child] = scratch_threat_counts_[index];
            std::copy_n(scratch_threats_[index].data(), kThreatListCapacity,
                        slot_threats_[index].data() + child * kThreatListCapacity);
        }
        slot_pieces_[child] = scratch_pieces_;
        return true;
    }
    if (slot_cursor_ == ply && slot_valid_[static_cast<std::size_t>(ply)] != 0 &&
        slot_keys_[static_cast<std::size_t>(ply)] == parent_key) {
        copy_slot_forward(static_cast<std::size_t>(ply), child);
        return true;
    }
    slot_valid_[child] = 0;
    slot_cursor_ = -1;
    scratch_valid_ = false;
    ++incremental_fallback_count_;
    return false;
}

void NnueWorker::on_make_move(const GameState& child, const MoveMetadata& metadata,
                              const int ply, const std::uint64_t parent_key) {
    if (!supports_incremental()) {
        return;
    }
    ensure_incremental_storage();
    const int child_slot = ply + 1;
    if (ply < 0 || static_cast<std::size_t>(child_slot) >= kIncrementalSlotCount) {
        slot_cursor_ = -1;
        scratch_valid_ = false;
        ++incremental_fallback_count_;
        return;
    }
    if (!prepare_child_slot(ply, child_slot, parent_key)) {
        return;
    }
    apply_move_deltas(child, metadata, static_cast<std::size_t>(child_slot));
    slot_keys_[static_cast<std::size_t>(child_slot)] = child.position_key();
    slot_valid_[static_cast<std::size_t>(child_slot)] = 1;
    slot_cursor_ = child_slot;
    ++incremental_make_count_;
}

void NnueWorker::on_unmake_move(const int child_ply) {
    if (!supports_incremental()) {
        return;
    }
    const int parent = child_ply - 1;
    slot_cursor_ = (parent >= 0 &&
                    static_cast<std::size_t>(parent) < kIncrementalSlotCount &&
                    slot_valid_[static_cast<std::size_t>(parent)] != 0) ? parent : -1;
}

void NnueWorker::on_make_null_move(const GameState& child, const int ply,
                                   const std::uint64_t parent_key) {
    if (!supports_incremental()) {
        return;
    }
    ensure_incremental_storage();
    const int child_slot = ply + 1;
    if (ply < 0 || static_cast<std::size_t>(child_slot) >= kIncrementalSlotCount) {
        slot_cursor_ = -1;
        scratch_valid_ = false;
        ++incremental_fallback_count_;
        return;
    }
    if (!prepare_child_slot(ply, child_slot, parent_key)) {
        return;
    }
    slot_keys_[static_cast<std::size_t>(child_slot)] = child.position_key();
    slot_valid_[static_cast<std::size_t>(child_slot)] = 1;
    slot_cursor_ = child_slot;
    ++incremental_make_count_;
}

void NnueWorker::on_unmake_null_move(const int child_ply) {
    on_unmake_move(child_ply);
}

namespace {

class NnueEvaluatorWorker final : public EvaluatorWorker {
public:
    explicit NnueEvaluatorWorker(std::shared_ptr<const NnueNetwork> weights)
        : worker_(std::move(weights)) {}

    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) override {
        return worker_.evaluate(state, perspective);
    }

    void on_make_move(const GameState& state, const MoveMetadata& metadata, const int ply,
                      const std::uint64_t parent_key) override {
        worker_.on_make_move(state, metadata, ply, parent_key);
    }
    void on_unmake_move(const int child_ply) override {
        worker_.on_unmake_move(child_ply);
    }
    void on_make_null_move(const GameState& state, const int ply,
                           const std::uint64_t parent_key) override {
        worker_.on_make_null_move(state, ply, parent_key);
    }
    void on_unmake_null_move(const int child_ply) override {
        worker_.on_unmake_null_move(child_ply);
    }

private:
    NnueWorker worker_;
};

} // namespace

namespace {

// A network is only usable when its manifest and arrays still validate. The
// public library seam must treat an invalid in-memory network the same way the
// load path treats a rejected file: fall back to the classical evaluator
// instead of returning silent zero scores.
bool network_is_usable(const NnueNetwork& network) {
    return !validate_manifest(network.manifest).has_value() && arrays_match_manifest(network);
}

} // namespace

NnueEvaluator::NnueEvaluator(std::shared_ptr<const NnueNetwork> weights)
    : NnueEvaluator(std::move(weights), std::make_shared<ClassicalEvaluator>()) {}

NnueEvaluator::NnueEvaluator(std::shared_ptr<const NnueNetwork> weights,
                             std::shared_ptr<const Evaluator> fallback)
    : weights_(std::move(weights)), fallback_(std::move(fallback)) {}

int NnueEvaluator::evaluate(const GameState& state, const Color perspective) const {
    if (!weights_ || !network_is_usable(*weights_)) {
        return fallback_ ? fallback_->evaluate(state, perspective) : 0;
    }
    // One-shot evaluation uses the stateless feature overload: the worker is a
    // temporary, so allocating and zeroing the per-ply incremental slots would
    // only add ~1 MB of pointless work to every fallback evaluation.
    NnueWorker worker(weights_);
    return worker.evaluate(EvaluationFeatureExtractor::extract(state), perspective);
}

std::unique_ptr<EvaluatorWorker> NnueEvaluator::create_worker() const {
    if (!weights_ || !network_is_usable(*weights_)) {
        return {};
    }
    return std::make_unique<NnueEvaluatorWorker>(weights_);
}

NnueWorker NnueEvaluator::make_worker() const {
    return NnueWorker(weights_);
}

std::shared_ptr<const Evaluator> maybe_wrap_gpu_nnue(
    const std::shared_ptr<const NnueNetwork>& network,
    std::shared_ptr<const Evaluator> cpu_evaluator) {
#if defined(KOI_GPU_INFERENCE_AVAILABLE) && KOI_GPU_INFERENCE_AVAILABLE
    const char* gpu_requested = std::getenv("KOI_GPU_NNUE");
    if (gpu_requested != nullptr && std::string_view(gpu_requested) != "0") {
        std::string gpu_error;
        std::unique_ptr<gpu::GpuNnueService> created =
            gpu::GpuNnueService::create(*network, gpu_error);
        if (created != nullptr) {
            std::fprintf(stderr, "koi-engine: GPU NNUE inference enabled.\n");
            return std::make_shared<gpu::GpuNnueEvaluator>(
                std::move(cpu_evaluator),
                std::shared_ptr<gpu::GpuNnueService>(std::move(created)));
        }
        std::fprintf(stderr,
                     "koi-engine: GPU NNUE unavailable (%s); using the CPU network.\n",
                     gpu_error.c_str());
    }
#else
    (void)network;
#endif
    return cpu_evaluator;
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
    const auto network = std::make_shared<const NnueNetwork>(std::move(*loaded));
    selection.evaluator = maybe_wrap_gpu_nnue(
        network, std::make_shared<NnueEvaluator>(network));
    selection.nnue_enabled = true;
    return selection;
}

} // namespace koi
