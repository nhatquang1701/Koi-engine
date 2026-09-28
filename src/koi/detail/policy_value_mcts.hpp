#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/move.hpp"

namespace koi::detail {

struct PolicyValueMctsEvaluation {
    float value = 0.0F;
    std::array<float, 3> wdl{0.0F, 1.0F, 0.0F};
};

struct PolicyValueMctsLeafRequest {
    const GameState* position = nullptr;
    const MoveMetadataList* legal_moves = nullptr;
    std::span<float> priors;
    PolicyValueMctsEvaluation evaluation{};
};

using PolicyValueMctsEvaluator = std::function<
    std::expected<PolicyValueMctsEvaluation, std::string>(
        const GameState&, const MoveMetadataList&, std::span<float> priors)>;
using PolicyValueMctsBatchEvaluator = std::function<
    std::expected<void, std::string>(std::span<PolicyValueMctsLeafRequest>)>;
using PolicyValueMctsStop = std::function<bool()>;

inline constexpr std::size_t kMaximumPolicyValueMctsBatchSize = 8;

struct PolicyValueMctsConfig {
    // 0 disables the explicit tree-ply cap; a fixed implementation safety cap
    // still prevents unbounded paths in malformed or cyclic positions.
    int max_depth = 0;
    std::size_t max_tree_nodes = 32'768;
    std::size_t max_tree_edges = 262'144;
    double cpuct = 1.5;
    bool enable_root_noise = false;
    double root_noise_alpha = 0.3;
    double root_noise_epsilon = 0.25;
    std::uint64_t root_noise_seed = 0;
};

struct PolicyValueMctsRootMove {
    Move move = Move::no_move();
    std::uint64_t visits = 0;
    double prior = 0.0;
    float value = 0.0F;
    std::array<float, 3> wdl{0.0F, 1.0F, 0.0F};
    std::vector<Move> pv;
};

struct PolicyValueMctsSnapshot {
    std::vector<PolicyValueMctsRootMove> root_moves;
    std::uint64_t simulations = 0;
    std::uint64_t leaf_evaluations = 0;
    int seldepth = 0;
    float value = 0.0F;
    std::array<float, 3> wdl{0.0F, 1.0F, 0.0F};
    bool tree_capped = false;
};

// One search-local PUCT tree. Position/rules stay in GameState; the tree stores
// only move edges and bounded statistics, with no transposition sharing. The
// evaluator is called only for newly visited leaves and must write priors in
// native legal-move order into caller-provided storage.
class PolicyValueMctsTree final {
public:
    PolicyValueMctsTree(GameState root, MoveMetadataList legal_root_moves,
                        PolicyValueMctsConfig config,
                        PolicyValueMctsEvaluator evaluator,
                        PolicyValueMctsBatchEvaluator batch_evaluator = {});
    ~PolicyValueMctsTree();

    PolicyValueMctsTree(const PolicyValueMctsTree&) = delete;
    PolicyValueMctsTree& operator=(const PolicyValueMctsTree&) = delete;
    PolicyValueMctsTree(PolicyValueMctsTree&&) noexcept;
    PolicyValueMctsTree& operator=(PolicyValueMctsTree&&) noexcept;

    [[nodiscard]] std::expected<void, std::string> initialize();
    [[nodiscard]] std::expected<bool, std::string> simulate(
        const std::atomic_bool& stop_requested,
        const PolicyValueMctsStop& should_stop = {});
    [[nodiscard]] std::expected<std::size_t, std::string> simulate_batch(
        std::size_t max_simulations, const std::atomic_bool& stop_requested,
        const PolicyValueMctsStop& should_stop = {});
    [[nodiscard]] PolicyValueMctsSnapshot snapshot(std::size_t multipv) const;
    void set_max_depth(int max_depth) noexcept;
    [[nodiscard]] std::uint64_t simulations() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi::detail
