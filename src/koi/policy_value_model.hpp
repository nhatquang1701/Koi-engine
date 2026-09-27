#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "koi/evaluation_features.hpp"
#include "koi/move.hpp"

namespace koi {

inline constexpr std::size_t kPolicyValueHeaderSize = 40;
inline constexpr std::uint32_t kPolicyValueModelVersion = 1;
inline constexpr std::uint32_t kPolicyValueFeatureSchemaId = 5;
inline constexpr std::uint32_t kPolicyValueActionEncodingId = 1;
inline constexpr std::size_t kPolicyValueHiddenSize = 64;
inline constexpr std::size_t kPolicyValueFeatureCount = 36'864;

enum class PolicyValueModelErrorCode : std::uint8_t {
    io_error,
    truncated_header,
    bad_magic,
    unsupported_version,
    unsupported_feature_schema,
    unsupported_action_encoding,
    invalid_dimensions,
    invalid_file_size,
    checksum_mismatch,
    non_finite_weight,
    allocation_failure,
    invalid_features,
    invalid_actions,
    output_size_mismatch,
};

struct PolicyValueModelError {
    PolicyValueModelErrorCode code = PolicyValueModelErrorCode::io_error;
    std::string message;
};

// Immutable CPU inference model for the separately versioned KOIPV1 container.
// Callers provide native legal moves and preallocated output storage so MCTS
// leaf evaluation never allocates a policy vector per node.
class PolicyValueModel final {
public:
    [[nodiscard]] static std::expected<PolicyValueModel, PolicyValueModelError> load(
        std::span<const std::uint8_t> container);
    [[nodiscard]] static std::expected<PolicyValueModel, PolicyValueModelError> load_file(
        const std::filesystem::path& path);

    // `sparse_features` must be the sorted side-to-move-relative output of
    // EvaluationFeatureExtractor::encode_sparse_v5. Priors are written in the
    // exact order of `legal_moves`; WDL and value use the side-to-move
    // perspective. Only action squares are vertically mirrored for Black.
    [[nodiscard]] std::expected<float, PolicyValueModelError> evaluate(
        const NnueSparseFeaturesV5& sparse_features, Color side_to_move,
        std::span<const Move> legal_moves, std::span<float> priors,
        std::array<float, 3>& wdl) const;

private:
    explicit PolicyValueModel(std::vector<float> weights) noexcept
        : weights_(std::move(weights)) {}

    std::vector<float> weights_;
};

} // namespace koi
