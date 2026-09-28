#include "koi/detail/policy_value_mcts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <utility>

namespace koi::detail {
namespace {

constexpr std::size_t kMaximumMctsPly = 256;
constexpr std::size_t kMaximumMctsTreeNodes = 65'536;
constexpr std::size_t kMaximumMctsTreeEdges = 524'288;
constexpr std::size_t kNoMctsNode = std::numeric_limits<std::size_t>::max();

[[nodiscard]] PolicyValueMctsEvaluation draw_evaluation() noexcept {
    return PolicyValueMctsEvaluation{0.0F, {0.0F, 1.0F, 0.0F}};
}

[[nodiscard]] PolicyValueMctsEvaluation checkmate_evaluation() noexcept {
    return PolicyValueMctsEvaluation{-1.0F, {0.0F, 0.0F, 1.0F}};
}

[[nodiscard]] PolicyValueMctsEvaluation flipped(
    PolicyValueMctsEvaluation evaluation) noexcept {
    evaluation.value = -evaluation.value;
    std::swap(evaluation.wdl[0], evaluation.wdl[2]);
    return evaluation;
}

[[nodiscard]] std::expected<PolicyValueMctsEvaluation, std::string> normalize_evaluation(
    PolicyValueMctsEvaluation evaluation, const std::span<float> priors) {
    if (!std::isfinite(evaluation.value) || evaluation.value < -1.00001F ||
        evaluation.value > 1.00001F) {
        return std::unexpected("policy/value evaluator returned an invalid value");
    }
    double wdl_sum = 0.0;
    for (const float probability : evaluation.wdl) {
        if (!std::isfinite(probability) || probability < 0.0F) {
            return std::unexpected("policy/value evaluator returned invalid WDL probabilities");
        }
        wdl_sum += static_cast<double>(probability);
    }
    if (!(wdl_sum > 0.0) || !std::isfinite(wdl_sum)) {
        return std::unexpected("policy/value evaluator returned an empty WDL distribution");
    }
    for (float& probability : evaluation.wdl) {
        probability = static_cast<float>(static_cast<double>(probability) / wdl_sum);
    }

    double prior_sum = 0.0;
    for (const float prior : priors) {
        if (!std::isfinite(prior) || prior < 0.0F) {
            return std::unexpected("policy/value evaluator returned invalid move priors");
        }
        prior_sum += static_cast<double>(prior);
    }
    if (!priors.empty() && (!(prior_sum > 0.0) || !std::isfinite(prior_sum))) {
        return std::unexpected("policy/value evaluator returned an empty policy distribution");
    }
    for (float& prior : priors) {
        prior = static_cast<float>(static_cast<double>(prior) / prior_sum);
    }
    evaluation.value = std::clamp(evaluation.value, -1.0F, 1.0F);
    return evaluation;
}

} // namespace

struct PolicyValueMctsTree::Impl {
    struct Node {
        std::uint64_t visits = 0;
        double value_sum = 0.0;
        std::array<double, 3> wdl_sum{};
        std::size_t first_edge = 0;
        std::size_t edge_count = 0;
        std::size_t depth = 0;
        std::uint64_t pending_visits = 0;
        PolicyValueMctsEvaluation leaf{};
        bool expanded = false;
        bool leaf_only = false;
        bool evaluation_pending = false;
    };

    struct Edge {
        MoveMetadata metadata{};
        double prior = 0.0;
        std::uint64_t visits = 0;
        double value_sum = 0.0;
        std::array<double, 3> wdl_sum{};
        std::size_t child = kNoMctsNode;
        std::uint64_t pending_visits = 0;
        double pending_value_sum = 0.0;
        PolicyValueMctsEvaluation cached_leaf{};
        bool has_cached_leaf = false;
        bool evaluation_pending = false;
    };

    GameState position;
    MoveMetadataList root_moves;
    PolicyValueMctsConfig config;
    PolicyValueMctsEvaluator evaluator;
    PolicyValueMctsBatchEvaluator batch_evaluator;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::uint64_t simulations = 0;
    std::uint64_t leaf_evaluations = 0;
    std::size_t maximum_depth = 0;
    PolicyValueMctsEvaluation root_prior{};
    bool root_claimable_draw = false;
    bool tree_capped = false;
    bool initialized = false;

    Impl(GameState root, MoveMetadataList legal_root_moves,
         PolicyValueMctsConfig search_config, PolicyValueMctsEvaluator evaluate,
         PolicyValueMctsBatchEvaluator evaluate_batch)
        : position(std::move(root)), root_moves(std::move(legal_root_moves)),
          config(search_config), evaluator(std::move(evaluate)),
          batch_evaluator(std::move(evaluate_batch)) {
        position.detach_mirror();
        config.max_tree_nodes = std::min(config.max_tree_nodes, kMaximumMctsTreeNodes);
        config.max_tree_edges = std::min(config.max_tree_edges, kMaximumMctsTreeEdges);
        if (config.max_depth > static_cast<int>(kMaximumMctsPly)) {
            config.max_depth = static_cast<int>(kMaximumMctsPly);
        }
        root_claimable_draw = position.is_claimable_draw();
    }

    [[nodiscard]] std::expected<PolicyValueMctsEvaluation, std::string> evaluate_position(
        const MoveMetadataList& legal_moves, const std::span<float> priors,
        bool& terminal) {
        terminal = false;
        if (legal_moves.empty()) {
            terminal = true;
            return position.in_check() ? checkmate_evaluation() : draw_evaluation();
        }
        if (position.is_forced_draw()) {
            terminal = true;
            return draw_evaluation();
        }
        if (!evaluator) {
            return std::unexpected("policy/value evaluator is not available");
        }
        std::expected<PolicyValueMctsEvaluation, std::string> evaluated =
            evaluator(position, legal_moves, priors);
        if (!evaluated.has_value()) {
            return std::unexpected(evaluated.error());
        }
        auto normalized = normalize_evaluation(*evaluated, priors);
        if (!normalized.has_value()) {
            return std::unexpected(normalized.error());
        }
        ++leaf_evaluations;
        PolicyValueMctsEvaluation value = *normalized;
        if (position.is_claimable_draw() && value.value < 0.0F) {
            value = draw_evaluation();
        }
        return value;
    }

    [[nodiscard]] std::expected<void, std::string> expand_node(
        const std::size_t node_index, const MoveMetadataList& legal_moves,
        const bool allow_children) {
        std::array<float, kMaximumLegalMoves> priors{};
        bool terminal = false;
        auto evaluated = evaluate_position(
            legal_moves, std::span<float>(priors.data(), legal_moves.size()), terminal);
        if (!evaluated.has_value()) {
            return std::unexpected(evaluated.error());
        }
        Node& node = nodes[node_index];
        node.expanded = true;
        node.leaf = *evaluated;
        if (terminal || !allow_children) {
            node.leaf_only = true;
            return {};
        }

        if (legal_moves.size() > config.max_tree_edges - edges.size()) {
            tree_capped = true;
            node.leaf_only = true;
            return {};
        }

        node.first_edge = edges.size();
        node.edge_count = legal_moves.size();
        for (std::size_t index = 0; index < legal_moves.size(); ++index) {
            Edge edge;
            edge.metadata = legal_moves[index];
            edge.prior = static_cast<double>(priors[index]);
            edges.push_back(edge);
        }
        return {};
    }
};

PolicyValueMctsTree::PolicyValueMctsTree(
    GameState root, MoveMetadataList legal_root_moves,
    PolicyValueMctsConfig config, PolicyValueMctsEvaluator evaluator,
    PolicyValueMctsBatchEvaluator batch_evaluator)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(legal_root_moves),
                                   config, std::move(evaluator),
                                   std::move(batch_evaluator))) {}

PolicyValueMctsTree::~PolicyValueMctsTree() = default;
PolicyValueMctsTree::PolicyValueMctsTree(PolicyValueMctsTree&&) noexcept = default;
PolicyValueMctsTree& PolicyValueMctsTree::operator=(PolicyValueMctsTree&&) noexcept = default;

void PolicyValueMctsTree::set_max_depth(const int max_depth) noexcept {
    impl_->config.max_depth = std::clamp(max_depth, 0, static_cast<int>(kMaximumMctsPly));
}

std::uint64_t PolicyValueMctsTree::simulations() const noexcept {
    return impl_->simulations;
}

std::expected<void, std::string> PolicyValueMctsTree::initialize() {
    if (impl_->initialized) {
        return std::unexpected("MCTS tree is already initialized");
    }
    if (impl_->config.max_tree_nodes == 0 || impl_->config.max_tree_edges == 0) {
        return std::unexpected("MCTS tree capacities must be positive");
    }
    if (impl_->config.max_depth < 0 || !std::isfinite(impl_->config.cpuct) ||
        impl_->config.cpuct < 0.0) {
        return std::unexpected("MCTS depth and PUCT configuration are invalid");
    }
    if (impl_->config.enable_root_noise &&
        (!std::isfinite(impl_->config.root_noise_alpha) ||
         impl_->config.root_noise_alpha <= 0.0 ||
         !std::isfinite(impl_->config.root_noise_epsilon) ||
         impl_->config.root_noise_epsilon < 0.0 ||
         impl_->config.root_noise_epsilon > 1.0)) {
        return std::unexpected("MCTS root-noise parameters are invalid");
    }
    if (impl_->root_moves.size() > impl_->config.max_tree_edges) {
        return std::unexpected("MCTS edge capacity cannot hold the legal root moves");
    }
    try {
        impl_->nodes.reserve(impl_->config.max_tree_nodes);
        impl_->edges.reserve(impl_->config.max_tree_edges);
        impl_->nodes.emplace_back();
    } catch (const std::bad_alloc&) {
        return std::unexpected("unable to allocate the bounded MCTS tree");
    }
    auto expanded = impl_->expand_node(0, impl_->root_moves, true);
    if (!expanded.has_value()) {
        return std::unexpected(expanded.error());
    }
    Impl::Node& root = impl_->nodes[0];
    if (impl_->config.enable_root_noise && root.edge_count > 0 &&
        impl_->config.root_noise_epsilon > 0.0) {
        std::mt19937_64 random(impl_->config.root_noise_seed);
        std::gamma_distribution<double> gamma(impl_->config.root_noise_alpha, 1.0);
        std::array<double, kMaximumLegalMoves> noise{};
        double noise_sum = 0.0;
        for (std::size_t index = 0; index < root.edge_count; ++index) {
            const double sample = gamma(random);
            if (!std::isfinite(sample) || sample < 0.0) {
                return std::unexpected("MCTS root-noise sampler returned an invalid sample");
            }
            noise[index] = sample;
            noise_sum += sample;
        }
        if (!(noise_sum > 0.0) || !std::isfinite(noise_sum)) {
            return std::unexpected("MCTS root-noise sampler returned an empty distribution");
        }
        const double prior_fraction = 1.0 - impl_->config.root_noise_epsilon;
        double mixed_sum = 0.0;
        for (std::size_t index = 0; index < root.edge_count; ++index) {
            Impl::Edge& edge = impl_->edges[root.first_edge + index];
            edge.prior = prior_fraction * edge.prior +
                impl_->config.root_noise_epsilon * noise[index] / noise_sum;
            mixed_sum += edge.prior;
        }
        if (!(mixed_sum > 0.0) || !std::isfinite(mixed_sum)) {
            return std::unexpected("MCTS root-noise mix returned an invalid distribution");
        }
        for (std::size_t index = 0; index < root.edge_count; ++index) {
            impl_->edges[root.first_edge + index].prior /= mixed_sum;
        }
    }
    root.visits = 1;
    root.value_sum = root.leaf.value;
    for (std::size_t index = 0; index < root.wdl_sum.size(); ++index) {
        root.wdl_sum[index] = root.leaf.wdl[index];
    }
    impl_->root_prior = root.leaf;
    impl_->initialized = true;
    return {};
}

std::expected<bool, std::string> PolicyValueMctsTree::simulate(
    const std::atomic_bool& stop_requested,
    const PolicyValueMctsStop& should_stop) {
    if (!impl_->initialized) {
        return std::unexpected("MCTS tree must be initialized before simulation");
    }
    if (stop_requested.load(std::memory_order_relaxed) ||
        impl_->nodes.empty() || impl_->nodes[0].leaf_only ||
        impl_->nodes[0].edge_count == 0) {
        return false;
    }

    std::array<std::size_t, kMaximumMctsPly + 1> path_nodes{};
    std::array<std::size_t, kMaximumMctsPly> path_edges{};
    path_nodes[0] = 0;
    std::size_t path_node_count = 1;
    std::size_t path_edge_count = 0;
    std::size_t moves_made = 0;
    std::size_t depth = 0;
    PolicyValueMctsEvaluation leaf{};
    bool has_leaf_node = false;

    const auto unmake_path = [this, &moves_made]() {
        while (moves_made > 0) {
            (void)impl_->position.unmake_move();
            --moves_made;
        }
    };

    for (;;) {
        bool interrupted = stop_requested.load(std::memory_order_relaxed);
        if (!interrupted && should_stop) {
            try {
                interrupted = should_stop();
            } catch (...) {
                unmake_path();
                return std::unexpected("MCTS stop predicate failed");
            }
        }
        if (interrupted) {
            unmake_path();
            return false;
        }
        const std::size_t node_index = path_nodes[path_node_count - 1];
        if (impl_->config.max_depth > 0 &&
            depth >= static_cast<std::size_t>(impl_->config.max_depth) &&
            impl_->nodes[node_index].expanded) {
            leaf = impl_->nodes[node_index].leaf;
            has_leaf_node = true;
            break;
        }
        if (!impl_->nodes[node_index].expanded) {
            MoveMetadataList legal_moves;
            impl_->position.legal_moves_with_metadata(legal_moves, false, false);
            const bool depth_frontier = impl_->config.max_depth > 0 &&
                depth >= static_cast<std::size_t>(impl_->config.max_depth);
            const bool hard_frontier = depth >= kMaximumMctsPly;
            auto expanded = impl_->expand_node(
                node_index, legal_moves, !depth_frontier && !hard_frontier);
            if (!expanded.has_value()) {
                unmake_path();
                return std::unexpected(expanded.error());
            }
        }

        Impl::Node& node = impl_->nodes[node_index];
        if (node.leaf_only || node.edge_count == 0) {
            leaf = node.leaf;
            has_leaf_node = true;
            break;
        }

        const double exploration_scale = std::sqrt(static_cast<double>(node.visits) + 1.0);
        std::size_t selected_edge_index = node.first_edge;
        double selected_score = -std::numeric_limits<double>::infinity();
        for (std::size_t offset = 0; offset < node.edge_count; ++offset) {
            const std::size_t edge_index = node.first_edge + offset;
            const Impl::Edge& edge = impl_->edges[edge_index];
            double exploitation = edge.visits == 0 ? 0.0 :
                edge.value_sum / static_cast<double>(edge.visits);
            if (node_index == 0 && impl_->root_claimable_draw) {
                // A claim is a zero-valued root action. Keep positive
                // continuations above it while preventing losing lines from
                // being preferred only because their value is less negative.
                exploitation = std::max(0.0, exploitation);
            }
            const double exploration = impl_->config.cpuct * edge.prior * exploration_scale /
                (1.0 + static_cast<double>(edge.visits));
            const double score = exploitation + exploration;
            if (score > selected_score) {
                selected_score = score;
                selected_edge_index = edge_index;
            }
        }
        if (path_edge_count >= path_edges.size()) {
            unmake_path();
            return std::unexpected("MCTS search path exceeded its fixed safety bound");
        }
        path_edges[path_edge_count++] = selected_edge_index;

        const MoveMetadata selected_move = impl_->edges[selected_edge_index].metadata;
        if (!impl_->position.make_search_move(selected_move)) {
            unmake_path();
            return std::unexpected("native GameState rejected an MCTS-generated legal move");
        }
        ++moves_made;
        ++depth;
        impl_->maximum_depth = std::max(impl_->maximum_depth, depth);

        Impl::Edge& selected_edge = impl_->edges[selected_edge_index];
        if (selected_edge.has_cached_leaf) {
            leaf = selected_edge.cached_leaf;
            has_leaf_node = false;
            break;
        }
        if (selected_edge.child == kNoMctsNode) {
            if (impl_->nodes.size() >= impl_->config.max_tree_nodes) {
                impl_->tree_capped = true;
                MoveMetadataList leaf_moves;
                impl_->position.legal_moves_with_metadata(leaf_moves, false, false);
                std::array<float, kMaximumLegalMoves> priors{};
                bool terminal = false;
                auto evaluated = impl_->evaluate_position(
                    leaf_moves, std::span<float>(priors.data(), leaf_moves.size()), terminal);
                if (!evaluated.has_value()) {
                    unmake_path();
                    return std::unexpected(evaluated.error());
                }
                selected_edge.cached_leaf = *evaluated;
                selected_edge.has_cached_leaf = true;
                leaf = *evaluated;
                has_leaf_node = false;
                (void)terminal;
                break;
            }
            try {
                Impl::Node child;
                child.depth = depth;
                selected_edge.child = impl_->nodes.size();
                impl_->nodes.push_back(child);
            } catch (const std::bad_alloc&) {
                unmake_path();
                return std::unexpected("unable to allocate an MCTS child node");
            }
        }
        if (path_node_count >= path_nodes.size()) {
            unmake_path();
            return std::unexpected("MCTS node path exceeded its fixed safety bound");
        }
        path_nodes[path_node_count++] = selected_edge.child;
    }

    if (has_leaf_node) {
        Impl::Node& leaf_node = impl_->nodes[path_nodes[path_node_count - 1]];
        ++leaf_node.visits;
        leaf_node.value_sum += static_cast<double>(leaf.value);
        for (std::size_t result = 0; result < leaf.wdl.size(); ++result) {
            leaf_node.wdl_sum[result] += leaf.wdl[result];
        }
    }

    PolicyValueMctsEvaluation parent_value = leaf;
    for (std::size_t edge_offset = path_edge_count; edge_offset > 0; --edge_offset) {
        const std::size_t edge_index = path_edges[edge_offset - 1];
        parent_value = flipped(parent_value);
        Impl::Edge& edge = impl_->edges[edge_index];
        ++edge.visits;
        edge.value_sum += parent_value.value;
        for (std::size_t result = 0; result < parent_value.wdl.size(); ++result) {
            edge.wdl_sum[result] += parent_value.wdl[result];
        }

        const std::size_t parent_path_index =
            std::min(edge_offset - 1, path_node_count - 1);
        Impl::Node& parent = impl_->nodes[path_nodes[parent_path_index]];
        ++parent.visits;
        parent.value_sum += parent_value.value;
        for (std::size_t result = 0; result < parent_value.wdl.size(); ++result) {
            parent.wdl_sum[result] += parent_value.wdl[result];
        }
    }
    unmake_path();
    ++impl_->simulations;
    return true;
}

std::expected<std::size_t, std::string> PolicyValueMctsTree::simulate_batch(
    std::size_t max_simulations, const std::atomic_bool& stop_requested,
    const PolicyValueMctsStop& should_stop) {
    if (!impl_->initialized) {
        return std::unexpected("MCTS tree must be initialized before simulation");
    }
    if (max_simulations == 0) {
        return std::size_t{0};
    }
    max_simulations = std::min(max_simulations, kMaximumPolicyValueMctsBatchSize);
    if (!impl_->batch_evaluator || max_simulations == 1) {
        std::size_t completed = 0;
        for (; completed < max_simulations; ++completed) {
            auto simulated = simulate(stop_requested, should_stop);
            if (!simulated.has_value()) {
                return std::unexpected(simulated.error());
            }
            if (!simulated.value()) {
                break;
            }
        }
        return completed;
    }
    if (impl_->nodes.empty() || impl_->nodes[0].leaf_only ||
        impl_->nodes[0].edge_count == 0) {
        return std::size_t{0};
    }

    enum class ExpansionTarget : std::uint8_t { none, node, cached_edge };
    struct PendingSimulation {
        std::array<std::size_t, kMaximumMctsPly + 1> path_nodes{};
        std::array<std::size_t, kMaximumMctsPly> path_edges{};
        std::size_t path_node_count = 1;
        std::size_t path_edge_count = 0;
        std::size_t depth = 0;
        std::size_t target_index = kNoMctsNode;
        ExpansionTarget target = ExpansionTarget::none;
        bool has_leaf_node = false;
        bool leaf_visit_reserved = false;
        bool requires_evaluation = false;
        bool allow_children = false;
        bool terminal = false;
        std::optional<GameState> leaf_position;
        MoveMetadataList legal_moves;
        std::array<float, kMaximumLegalMoves> priors{};
        PolicyValueMctsEvaluation leaf{};
    };

    std::vector<PendingSimulation> pending;
    try {
        pending.reserve(max_simulations);
    } catch (const std::bad_alloc&) {
        return std::unexpected("unable to allocate a bounded MCTS evaluation batch");
    }

    const auto check_stop = [&]() -> std::expected<bool, std::string> {
        if (stop_requested.load(std::memory_order_relaxed)) {
            return true;
        }
        if (!should_stop) {
            return false;
        }
        try {
            return should_stop();
        } catch (...) {
            return std::unexpected("MCTS stop predicate failed");
        }
    };

    const auto clear_reservations = [this](PendingSimulation& simulation) noexcept {
        for (std::size_t index = 0; index < simulation.path_edge_count; ++index) {
            Impl::Edge& edge = impl_->edges[simulation.path_edges[index]];
            if (edge.pending_visits > 0) {
                --edge.pending_visits;
                edge.pending_value_sum += 1.0;
            }
            Impl::Node& parent = impl_->nodes[simulation.path_nodes[index]];
            if (parent.pending_visits > 0) {
                --parent.pending_visits;
            }
        }
        if (simulation.leaf_visit_reserved && simulation.path_node_count > 0) {
            Impl::Node& leaf = impl_->nodes[simulation.path_nodes[
                simulation.path_node_count - 1]];
            if (leaf.pending_visits > 0) {
                --leaf.pending_visits;
            }
        }
        if (simulation.target == ExpansionTarget::node &&
            simulation.target_index < impl_->nodes.size()) {
            impl_->nodes[simulation.target_index].evaluation_pending = false;
        } else if (simulation.target == ExpansionTarget::cached_edge &&
                   simulation.target_index < impl_->edges.size()) {
            impl_->edges[simulation.target_index].evaluation_pending = false;
        }
    };
    const auto rollback_batch = [&] {
        for (PendingSimulation& simulation : pending) {
            clear_reservations(simulation);
        }
    };
    const auto unmake_path = [this](std::size_t& moves_made) noexcept {
        while (moves_made > 0) {
            (void)impl_->position.unmake_move();
            --moves_made;
        }
    };

    for (std::size_t slot = 0; slot < max_simulations; ++slot) {
        auto stopped = check_stop();
        if (!stopped.has_value()) {
            rollback_batch();
            return std::unexpected(stopped.error());
        }
        if (stopped.value()) {
            rollback_batch();
            return std::size_t{0};
        }

        PendingSimulation candidate;
        candidate.path_nodes[0] = 0;
        std::size_t moves_made = 0;
        bool complete = false;
        bool blocked = false;
        bool deadline_reached = false;
        std::string candidate_error;

        for (;;) {
            stopped = check_stop();
            if (!stopped.has_value()) {
                candidate_error = stopped.error();
                break;
            }
            if (stopped.value()) {
                blocked = true;
                deadline_reached = true;
                break;
            }

            const std::size_t node_index =
                candidate.path_nodes[candidate.path_node_count - 1];
            Impl::Node& node = impl_->nodes[node_index];
            if (node.evaluation_pending) {
                blocked = true;
                break;
            }
            if (impl_->config.max_depth > 0 &&
                candidate.depth >= static_cast<std::size_t>(impl_->config.max_depth) &&
                node.expanded) {
                candidate.leaf = node.leaf;
                candidate.has_leaf_node = true;
                complete = true;
                break;
            }
            if (!node.expanded) {
                candidate.target = ExpansionTarget::node;
                candidate.target_index = node_index;
                candidate.has_leaf_node = true;
                candidate.leaf_visit_reserved = true;
                candidate.allow_children =
                    !(impl_->config.max_depth > 0 &&
                      candidate.depth >= static_cast<std::size_t>(impl_->config.max_depth)) &&
                    candidate.depth < kMaximumMctsPly;
                impl_->position.legal_moves_with_metadata(
                    candidate.legal_moves, false, false);
                if (candidate.legal_moves.empty()) {
                    candidate.terminal = true;
                    candidate.leaf = impl_->position.in_check() ?
                        checkmate_evaluation() : draw_evaluation();
                } else if (impl_->position.is_forced_draw()) {
                    candidate.terminal = true;
                    candidate.leaf = draw_evaluation();
                } else {
                    try {
                        candidate.leaf_position.emplace(impl_->position);
                    } catch (const std::bad_alloc&) {
                        candidate_error =
                            "unable to snapshot a bounded MCTS leaf position";
                        break;
                    }
                    candidate.requires_evaluation = true;
                }
                node.evaluation_pending = true;
                complete = true;
                break;
            }

            if (node.leaf_only || node.edge_count == 0) {
                candidate.leaf = node.leaf;
                candidate.has_leaf_node = true;
                complete = true;
                break;
            }

            const double exploration_scale = std::sqrt(
                static_cast<double>(node.visits + node.pending_visits) + 1.0);
            std::size_t selected_edge_index = node.first_edge;
            double selected_score = -std::numeric_limits<double>::infinity();
            for (std::size_t offset = 0; offset < node.edge_count; ++offset) {
                const std::size_t edge_index = node.first_edge + offset;
                const Impl::Edge& edge = impl_->edges[edge_index];
                double exploitation = edge.visits == 0 ? 0.0 :
                    edge.value_sum / static_cast<double>(edge.visits);
                if (node_index == 0 && impl_->root_claimable_draw) {
                    exploitation = std::max(0.0, exploitation);
                }
                const std::uint64_t effective_visits =
                    edge.visits + edge.pending_visits;
                if (effective_visits > 0 && edge.pending_visits > 0) {
                    exploitation += edge.pending_value_sum /
                        static_cast<double>(effective_visits);
                }
                const double exploration = impl_->config.cpuct * edge.prior *
                    exploration_scale / (1.0 + static_cast<double>(effective_visits));
                const double score = exploitation + exploration;
                if (score > selected_score) {
                    selected_score = score;
                    selected_edge_index = edge_index;
                }
            }

            if (candidate.path_edge_count >= candidate.path_edges.size()) {
                candidate_error = "MCTS search path exceeded its fixed safety bound";
                break;
            }
            Impl::Edge& selected_edge = impl_->edges[selected_edge_index];
            if (selected_edge.evaluation_pending) {
                blocked = true;
                break;
            }
            if (!impl_->position.make_search_move(selected_edge.metadata)) {
                candidate_error =
                    "native GameState rejected an MCTS-generated legal move";
                break;
            }
            ++moves_made;
            candidate.path_edges[candidate.path_edge_count++] = selected_edge_index;
            ++selected_edge.pending_visits;
            selected_edge.pending_value_sum -= 1.0;
            ++node.pending_visits;
            ++candidate.depth;

            if (selected_edge.has_cached_leaf) {
                candidate.leaf = selected_edge.cached_leaf;
                candidate.has_leaf_node = false;
                complete = true;
                break;
            }

            if (selected_edge.child == kNoMctsNode &&
                impl_->nodes.size() >= impl_->config.max_tree_nodes) {
                candidate.target = ExpansionTarget::cached_edge;
                candidate.target_index = selected_edge_index;
                candidate.has_leaf_node = false;
                candidate.legal_moves.resize(0);
                impl_->position.legal_moves_with_metadata(
                    candidate.legal_moves, false, false);
                if (candidate.legal_moves.empty()) {
                    candidate.terminal = true;
                    candidate.leaf = impl_->position.in_check() ?
                        checkmate_evaluation() : draw_evaluation();
                } else if (impl_->position.is_forced_draw()) {
                    candidate.terminal = true;
                    candidate.leaf = draw_evaluation();
                } else {
                    try {
                        candidate.leaf_position.emplace(impl_->position);
                    } catch (const std::bad_alloc&) {
                        candidate_error =
                            "unable to snapshot a bounded MCTS leaf position";
                        break;
                    }
                    candidate.requires_evaluation = true;
                }
                selected_edge.evaluation_pending = true;
                complete = true;
                break;
            }

            if (selected_edge.child == kNoMctsNode) {
                try {
                    Impl::Node child;
                    child.depth = candidate.depth;
                    selected_edge.child = impl_->nodes.size();
                    impl_->nodes.push_back(child);
                } catch (const std::bad_alloc&) {
                    candidate_error = "unable to allocate an MCTS child node";
                    break;
                }
            }
            if (candidate.path_node_count >= candidate.path_nodes.size()) {
                candidate_error = "MCTS node path exceeded its fixed safety bound";
                break;
            }
            candidate.path_nodes[candidate.path_node_count++] = selected_edge.child;
        }

        unmake_path(moves_made);
        if (!candidate_error.empty()) {
            clear_reservations(candidate);
            rollback_batch();
            return std::unexpected(std::move(candidate_error));
        }
        if (blocked || !complete) {
            clear_reservations(candidate);
            if (stop_requested.load(std::memory_order_relaxed) || deadline_reached) {
                rollback_batch();
                return std::size_t{0};
            }
            break;
        }
        if (candidate.has_leaf_node && candidate.leaf_visit_reserved) {
            ++impl_->nodes[candidate.path_nodes[
                candidate.path_node_count - 1]].pending_visits;
        }
        pending.push_back(std::move(candidate));
    }

    if (pending.empty()) {
        return std::size_t{0};
    }

    std::vector<PolicyValueMctsLeafRequest> requests;
    std::vector<std::size_t> request_to_pending;
    try {
        requests.reserve(pending.size());
        request_to_pending.reserve(pending.size());
        for (std::size_t index = 0; index < pending.size(); ++index) {
            PendingSimulation& simulation = pending[index];
            if (!simulation.requires_evaluation) {
                continue;
            }
            if (!simulation.leaf_position.has_value()) {
                rollback_batch();
                return std::unexpected("MCTS batch leaf is missing its position snapshot");
            }
            requests.push_back(PolicyValueMctsLeafRequest{
                &*simulation.leaf_position,
                &simulation.legal_moves,
                std::span<float>(simulation.priors.data(), simulation.legal_moves.size()),
                {},
            });
            request_to_pending.push_back(index);
        }
    } catch (const std::bad_alloc&) {
        rollback_batch();
        return std::unexpected("unable to stage bounded MCTS leaf requests");
    }

    if (!requests.empty()) {
        std::expected<void, std::string> evaluated;
        try {
            evaluated = impl_->batch_evaluator(
                std::span<PolicyValueMctsLeafRequest>(requests));
        } catch (const std::exception& error) {
            rollback_batch();
            return std::unexpected("MCTS batch evaluator failed: " +
                                   std::string(error.what()));
        } catch (...) {
            rollback_batch();
            return std::unexpected("MCTS batch evaluator failed with an unknown error");
        }
        if (!evaluated.has_value()) {
            rollback_batch();
            return std::unexpected(evaluated.error());
        }
        for (std::size_t request_index = 0;
             request_index < requests.size(); ++request_index) {
            PolicyValueMctsLeafRequest& request = requests[request_index];
            auto normalized = normalize_evaluation(request.evaluation, request.priors);
            if (!normalized.has_value()) {
                rollback_batch();
                return std::unexpected(normalized.error());
            }
            PendingSimulation& simulation = pending[request_to_pending[request_index]];
            simulation.leaf = *normalized;
            if (simulation.leaf_position->is_claimable_draw() &&
                simulation.leaf.value < 0.0F) {
                simulation.leaf = draw_evaluation();
            }
        }
    }

    auto stopped = check_stop();
    if (!stopped.has_value()) {
        rollback_batch();
        return std::unexpected(stopped.error());
    }
    if (stopped.value()) {
        rollback_batch();
        return std::size_t{0};
    }

    const auto expand_from_pending = [this](PendingSimulation& simulation) {
        if (simulation.target == ExpansionTarget::node) {
            Impl::Node& node = impl_->nodes[simulation.target_index];
            node.evaluation_pending = false;
            node.expanded = true;
            node.leaf = simulation.leaf;
            if (simulation.terminal || !simulation.allow_children) {
                node.leaf_only = true;
                return;
            }
            if (simulation.legal_moves.size() >
                impl_->config.max_tree_edges - impl_->edges.size()) {
                impl_->tree_capped = true;
                node.leaf_only = true;
                return;
            }
            node.first_edge = impl_->edges.size();
            node.edge_count = simulation.legal_moves.size();
            for (std::size_t index = 0; index < node.edge_count; ++index) {
                Impl::Edge edge;
                edge.metadata = simulation.legal_moves[index];
                edge.prior = static_cast<double>(simulation.priors[index]);
                impl_->edges.push_back(edge);
            }
        } else if (simulation.target == ExpansionTarget::cached_edge) {
            Impl::Edge& edge = impl_->edges[simulation.target_index];
            edge.evaluation_pending = false;
            edge.cached_leaf = simulation.leaf;
            edge.has_cached_leaf = true;
            if (impl_->nodes.size() >= impl_->config.max_tree_nodes) {
                impl_->tree_capped = true;
            }
        }
    };

    for (PendingSimulation& simulation : pending) {
        expand_from_pending(simulation);
        impl_->maximum_depth = std::max(impl_->maximum_depth, simulation.depth);
    }
    impl_->leaf_evaluations += requests.size();

    const auto back_up = [this](PendingSimulation& simulation) {
        if (simulation.has_leaf_node) {
            Impl::Node& leaf_node = impl_->nodes[
                simulation.path_nodes[simulation.path_node_count - 1]];
            ++leaf_node.visits;
            leaf_node.value_sum += static_cast<double>(simulation.leaf.value);
            for (std::size_t result = 0; result < simulation.leaf.wdl.size(); ++result) {
                leaf_node.wdl_sum[result] += simulation.leaf.wdl[result];
            }
        }
        PolicyValueMctsEvaluation parent_value = simulation.leaf;
        for (std::size_t offset = simulation.path_edge_count; offset > 0; --offset) {
            const std::size_t edge_index = simulation.path_edges[offset - 1];
            parent_value = flipped(parent_value);
            Impl::Edge& edge = impl_->edges[edge_index];
            ++edge.visits;
            edge.value_sum += parent_value.value;
            for (std::size_t result = 0; result < parent_value.wdl.size(); ++result) {
                edge.wdl_sum[result] += parent_value.wdl[result];
            }

            Impl::Node& parent = impl_->nodes[simulation.path_nodes[offset - 1]];
            ++parent.visits;
            parent.value_sum += parent_value.value;
            for (std::size_t result = 0; result < parent_value.wdl.size(); ++result) {
                parent.wdl_sum[result] += parent_value.wdl[result];
            }
        }
    };

    for (PendingSimulation& simulation : pending) {
        clear_reservations(simulation);
        back_up(simulation);
        ++impl_->simulations;
    }
    return pending.size();
}

PolicyValueMctsSnapshot PolicyValueMctsTree::snapshot(const std::size_t multipv) const {
    PolicyValueMctsSnapshot result;
    if (!impl_->initialized || impl_->nodes.empty()) {
        return result;
    }
    result.simulations = impl_->simulations;
    result.leaf_evaluations = impl_->leaf_evaluations;
    result.seldepth = static_cast<int>(impl_->maximum_depth);
    result.tree_capped = impl_->tree_capped;

    const Impl::Node& root = impl_->nodes[0];
    if (root.visits > 0) {
        result.value = static_cast<float>(root.value_sum / static_cast<double>(root.visits));
        for (std::size_t index = 0; index < result.wdl.size(); ++index) {
            result.wdl[index] = static_cast<float>(
                root.wdl_sum[index] / static_cast<double>(root.visits));
        }
    } else {
        result.value = impl_->root_prior.value;
        result.wdl = impl_->root_prior.wdl;
    }
    if (impl_->root_claimable_draw && result.value < 0.0F) {
        result.value = 0.0F;
        result.wdl = {0.0F, 1.0F, 0.0F};
    }

    if (root.edge_count == 0 || multipv == 0) {
        return result;
    }
    std::vector<std::size_t> order;
    try {
        order.reserve(root.edge_count);
        for (std::size_t index = 0; index < root.edge_count; ++index) {
            order.push_back(root.first_edge + index);
        }
    } catch (const std::bad_alloc&) {
        // A snapshot is diagnostic/publication data; the search result remains
        // available from the tree even if this optional ranking allocation
        // cannot be made.
        return result;
    }
    std::stable_sort(order.begin(), order.end(), [this](const std::size_t left,
                                                        const std::size_t right) {
        const Impl::Edge& a = impl_->edges[left];
        const Impl::Edge& b = impl_->edges[right];
        if (impl_->root_claimable_draw) {
            const double a_value = a.visits == 0 ? impl_->root_prior.value :
                a.value_sum / static_cast<double>(a.visits);
            const double b_value = b.visits == 0 ? impl_->root_prior.value :
                b.value_sum / static_cast<double>(b.visits);
            const double a_claim_adjusted = std::max(0.0, a_value);
            const double b_claim_adjusted = std::max(0.0, b_value);
            if (a_claim_adjusted != b_claim_adjusted) {
                return a_claim_adjusted > b_claim_adjusted;
            }
        }
        if (a.visits != b.visits) {
            return a.visits > b.visits;
        }
        return a.prior > b.prior;
    });

    const std::size_t lines = std::min(multipv, order.size());
    result.root_moves.reserve(lines);
    for (std::size_t line_index = 0; line_index < lines; ++line_index) {
        const Impl::Edge& root_edge = impl_->edges[order[line_index]];
        PolicyValueMctsRootMove line;
        line.move = root_edge.metadata.move;
        line.visits = root_edge.visits;
        line.prior = root_edge.prior;
        line.value = root_edge.visits == 0 ? impl_->root_prior.value :
            static_cast<float>(root_edge.value_sum / static_cast<double>(root_edge.visits));
        if (root_edge.visits == 0) {
            line.wdl = impl_->root_prior.wdl;
        } else {
            for (std::size_t index = 0; index < line.wdl.size(); ++index) {
                line.wdl[index] = static_cast<float>(
                    root_edge.wdl_sum[index] / static_cast<double>(root_edge.visits));
            }
        }
        if (impl_->root_claimable_draw && line.value < 0.0F) {
            line.value = 0.0F;
            line.wdl = {0.0F, 1.0F, 0.0F};
        }
        line.pv.push_back(line.move);
        std::size_t node_index = root_edge.child;
        while (node_index != kNoMctsNode && node_index < impl_->nodes.size()) {
            const Impl::Node& node = impl_->nodes[node_index];
            if (!node.expanded || node.leaf_only || node.edge_count == 0 ||
                line.pv.size() >= kMaximumMctsPly) {
                break;
            }
            std::size_t best_edge = node.first_edge;
            for (std::size_t offset = 1; offset < node.edge_count; ++offset) {
                const std::size_t candidate = node.first_edge + offset;
                const Impl::Edge& current = impl_->edges[best_edge];
                const Impl::Edge& next = impl_->edges[candidate];
                if (next.visits > current.visits ||
                    (next.visits == current.visits && next.prior > current.prior)) {
                    best_edge = candidate;
                }
            }
            const Impl::Edge& edge = impl_->edges[best_edge];
            line.pv.push_back(edge.metadata.move);
            node_index = edge.child;
        }
        result.root_moves.push_back(std::move(line));
    }
    return result;
}

} // namespace koi::detail
