#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "koi/evaluation_features.hpp"
#include "koi/evaluator.hpp"

namespace koi {

inline constexpr std::string_view kKoiNnueMagic = "KOI-NNUE";
inline constexpr std::uint32_t kKoiNnueFormatVersion = 2;
// Version 3 keeps the v2 tensor layout but stores explicit fixed-point shifts
// (hidden, bottleneck, output) in the header.  Trained models whose weight
// magnitudes are far from the activation range can then be quantized without
// collapsing to zero, which the single-scale v2 chain cannot avoid.
inline constexpr std::uint32_t kKoiNnuePerspectiveV3FormatVersion = 3;
// Version 4 replaces the 960-input piece-square features with
// halfka-king-bucket-v1 (9216 inputs), CReLU pair products, and eight
// piece-count output buckets.  It keeps the explicit fixed-point shift idea
// from v3 but stores only the first-layer and output shifts.
inline constexpr std::uint32_t kKoiNnueHalfkaKingBucketV1FormatVersion = 4;
inline constexpr std::uint32_t kKoiNnueHalfkaKingBucketV1FeatureCount =
    static_cast<std::uint32_t>(kNnueHalfkaKingBucketV1FeatureCount);
inline constexpr std::uint32_t kKoiNnueOutputBucketCount = 8;
inline constexpr std::uint32_t kKoiNnueMinimumHiddenUnits = 32;
inline constexpr std::uint32_t kKoiNnueMaximumHiddenUnits = 8192;
// Upper bound for every explicit fixed-point right shift stored in a v3/v4
// header.  Values beyond this cannot be expressed meaningfully by the integer
// pipelines the containers target.
inline constexpr std::uint8_t kKoiNnueMaximumShift = 20;
inline constexpr std::string_view kKoiNnueHalfkaKingBucketV1FeatureSet =
    kNnueHalfkaKingBucketV1FeatureSet;
inline constexpr std::uint32_t kKoiNnueFeatureCount =
    static_cast<std::uint32_t>(kNnuePieceSquareV1FeatureCount);
inline constexpr std::uint32_t kKoiNnuePieceSquareKingPawnV2FeatureCount =
    static_cast<std::uint32_t>(kNnuePieceSquareKingPawnV2FeatureCount);
using NnueLayerSizes = std::array<std::uint32_t, 4>;
inline constexpr NnueLayerSizes kKoiNnueLayerSizes{768, 128, 32, 1};
inline constexpr NnueLayerSizes kKoiNnuePieceSquareKingPawnV2LayerSizes{960, 256, 32, 1};
inline constexpr std::uint32_t kKoiNnueV2FeatureCount =
    kKoiNnuePieceSquareKingPawnV2FeatureCount;
inline constexpr NnueLayerSizes kKoiNnueV2LayerSizes =
    kKoiNnuePieceSquareKingPawnV2LayerSizes;
inline constexpr std::string_view kKoiNnueFeatureSet = kNnuePieceSquareV1FeatureSet;
inline constexpr std::string_view kKoiNnuePieceSquareKingPawnV2FeatureSet =
    kNnuePieceSquareKingPawnV2FeatureSet;
inline constexpr std::string_view kKoiNnueV2FeatureSet =
    kKoiNnuePieceSquareKingPawnV2FeatureSet;
inline constexpr std::string_view kKoiNnueQuantization = "int16/int8";
inline constexpr std::int32_t kKoiNnueClippedReluMaximum = 127;

enum class NnueErrorCode : std::uint8_t {
    io_error,
    empty_container,
    bad_magic,
    unsupported_version,
    malformed_manifest,
    invalid_dimensions,
    unsupported_feature_set,
    unsupported_quantization,
    invalid_file_size,
    checksum_mismatch,
};

struct NnueError {
    NnueErrorCode code = NnueErrorCode::malformed_manifest;
    std::string message;
};

struct NnueManifest {
    std::string magic;
    std::uint32_t version = kKoiNnueFormatVersion;
    NnueLayerSizes layer_sizes = kKoiNnueLayerSizes;
    std::string feature_set;
    std::string quantization = std::string(kKoiNnueQuantization);
    std::uint64_t payload_size = 0;
    std::array<std::uint8_t, 32> network_sha256{};
};

struct NnueNetwork {
    NnueManifest manifest;
    std::vector<std::int16_t> feature_weights;
    std::vector<std::int32_t> hidden_bias;
    std::vector<std::int8_t> bottleneck_weights;
    std::vector<std::int32_t> bottleneck_bias;
    std::vector<std::int8_t> output_weights;
    std::int32_t output_bias = 0;

    // Fixed-point right shifts used by the v3 integer pipeline.  Version 1 and
    // version 2 containers leave them at zero, which reproduces the original
    // v2 activation-only quantization exactly.
    std::uint8_t hidden_shift = 0;
    std::uint8_t bottleneck_shift = 0;
    std::uint8_t output_shift = 0;

    [[nodiscard]] static NnueNetwork synthetic();
    [[nodiscard]] static NnueNetwork synthetic_v2();
    // Minimal-width v4 network for container and inference fixtures.  Trained
    // networks default to 1024 hidden units; the fixture keeps the payload
    // small while still exercising the pair-product and per-bucket layout.
    [[nodiscard]] static NnueNetwork synthetic_v4();
};

class NnueLoader final {
public:
    [[nodiscard]] static std::expected<NnueNetwork, NnueError> load(
        std::span<const std::uint8_t> container);
    [[nodiscard]] static std::expected<NnueNetwork, NnueError> load_file(
        const std::filesystem::path& path);
    [[nodiscard]] static std::expected<std::vector<std::uint8_t>, NnueError> serialize(
        const NnueNetwork& network);
};

struct NnueAccumulator {
    std::vector<std::int16_t> values;
    std::vector<std::int16_t> bottleneck_values;
};

enum class NnueInferencePath : std::uint8_t {
    automatic,
    scalar,
    avx2_compatible,
};

class NnueWorker final {
public:
    explicit NnueWorker(std::shared_ptr<const NnueNetwork> weights);

    [[nodiscard]] int evaluate(const EvaluationFeatures&, Color perspective,
                               NnueInferencePath path = NnueInferencePath::automatic);
    [[nodiscard]] int evaluate(const GameState&, Color perspective,
                               NnueInferencePath path = NnueInferencePath::automatic);
    [[nodiscard]] const NnueAccumulator& accumulator() const noexcept { return accumulator_; }

    // Advisory lifecycle hooks.  For v4 networks the worker keeps one hidden
    // accumulator per perspective and updates it with feature deltas instead
    // of recomputing.  Every hook and every evaluation verifies the position
    // key before trusting cached state, so a skipped notification can only
    // force a full refresh, never an incorrect score.
    void on_make_move(const GameState&, const MoveMetadata&, int ply,
                      std::uint64_t parent_key);
    void on_unmake_move(int child_ply);
    void on_make_null_move(const GameState&, int ply, std::uint64_t parent_key);
    void on_unmake_null_move(int child_ply);

    [[nodiscard]] std::size_t incremental_make_count() const noexcept {
        return incremental_make_count_;
    }
    [[nodiscard]] std::size_t incremental_fallback_count() const noexcept {
        return incremental_fallback_count_;
    }

private:
    // Root slot plus the deepest search line the engine can reach.  Slots past
    // the cursor are never trusted, so an overrun only costs a refresh.
    static constexpr std::size_t kIncrementalSlotCount = 132;
    static constexpr std::size_t kIncrementalPerspectives = 2;

    [[nodiscard]] bool supports_incremental() const noexcept;
    void ensure_incremental_storage();
    [[nodiscard]] bool prepare_child_slot(int ply, int child_slot,
                                          std::uint64_t parent_key);
    void copy_slot_forward(std::size_t from_slot, std::size_t to_slot);
    void refresh_slot_perspective(const EvaluationFeatures&, Color perspective,
                                  std::size_t slot);
    void apply_feature_delta(std::size_t perspective, std::size_t slot,
                             std::uint16_t index, int sign);
    void apply_move_deltas(const GameState&, const MoveMetadata&, std::size_t slot);

    std::shared_ptr<const NnueNetwork> weights_;
    NnueAccumulator accumulator_;

    std::size_t hidden_units_ = 0;
    std::array<std::vector<std::int32_t>, kIncrementalPerspectives> slot_values_;
    std::array<std::vector<std::uint8_t>, kIncrementalPerspectives> slot_buckets_;
    std::vector<std::uint64_t> slot_keys_;
    std::vector<std::uint8_t> slot_valid_;
    int slot_cursor_ = -1;
    std::array<std::vector<std::int32_t>, kIncrementalPerspectives> scratch_values_;
    std::array<std::uint8_t, kIncrementalPerspectives> scratch_buckets_{};
    std::uint64_t scratch_key_ = 0;
    bool scratch_valid_ = false;
    bool storage_ready_ = false;
    std::size_t incremental_make_count_ = 0;
    std::size_t incremental_fallback_count_ = 0;
};

class NnueEvaluator final : public Evaluator {
public:
    explicit NnueEvaluator(std::shared_ptr<const NnueNetwork> weights);
    NnueEvaluator(std::shared_ptr<const NnueNetwork> weights,
                  std::shared_ptr<const Evaluator> fallback);

    [[nodiscard]] int evaluate(const GameState&, Color perspective) const override;
    [[nodiscard]] bool supports_concurrent_evaluation() const noexcept override { return true; }
    [[nodiscard]] std::unique_ptr<EvaluatorWorker> create_worker() const override;
    [[nodiscard]] NnueWorker make_worker() const;
    [[nodiscard]] bool enabled() const noexcept { return static_cast<bool>(weights_); }

private:
    std::shared_ptr<const NnueNetwork> weights_;
    std::shared_ptr<const Evaluator> fallback_;
};

// Library-only selection seam. The engine entry points (main.cpp and
// uci_controller.cpp) construct ClassicalEvaluator directly, and the
// advertised UCI option list deliberately exposes no network-path setting;
// this helper remains for library consumers and tests.
struct EvaluatorSelection {
    std::shared_ptr<const Evaluator> evaluator;
    bool nnue_enabled = false;
    std::optional<NnueError> nnue_error;
};

[[nodiscard]] EvaluatorSelection make_evaluator(
    std::optional<std::filesystem::path> nnue_path = std::nullopt);

} // namespace koi
