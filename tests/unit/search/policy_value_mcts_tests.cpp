#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "koi/detail/policy_value_mcts.hpp"
#include "koi/game_state.hpp"
#include "koi_test_support.hpp"

namespace koi {
using detail::PolicyValueMctsConfig;
using detail::PolicyValueMctsEvaluation;
using detail::PolicyValueMctsEvaluator;
using detail::PolicyValueMctsSnapshot;
using detail::PolicyValueMctsTree;
} // namespace koi

namespace {

using koi::test::require;

[[nodiscard]] koi::GameState require_state(const std::string_view fen) {
    return koi::test::require_value(
        koi::GameState::from_fen(fen), "MCTS fixture FEN must parse");
}

[[nodiscard]] koi::MoveMetadataList legal_root_moves(koi::GameState& state) {
    koi::MoveMetadataList moves;
    state.legal_moves_with_metadata(moves, false, false);
    return moves;
}

[[nodiscard]] koi::PolicyValueMctsEvaluator uniform_draw_evaluator() {
    return [](const koi::GameState&, const koi::MoveMetadataList& legal,
              const std::span<float> priors)
               -> std::expected<koi::PolicyValueMctsEvaluation, std::string> {
        require(!legal.empty() && priors.size() == legal.size(),
                "MCTS evaluator must receive aligned non-empty legal actions and priors");
        const float prior = 1.0F / static_cast<float>(legal.size());
        std::fill(priors.begin(), priors.end(), prior);
        return koi::PolicyValueMctsEvaluation{
            0.0F, {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}};
    };
}

void test_mcts_visit_limit_depth_cap_and_deterministic_root_choice() {
    koi::GameState root = koi::GameState::startpos();
    const auto preferred = koi::Move::parse_uci("e2e4");
    require(preferred.has_value(), "the preferred MCTS fixture move must parse");
    const koi::MoveMetadataList moves = legal_root_moves(root);
    auto evaluator = [preferred = *preferred](
                         const koi::GameState&, const koi::MoveMetadataList& legal,
                         const std::span<float> priors)
                         -> std::expected<koi::PolicyValueMctsEvaluation, std::string> {
        if (legal.empty() || priors.size() != legal.size()) {
            return std::unexpected("invalid MCTS evaluator input");
        }
        if (legal.size() == 1) {
            priors[0] = 1.0F;
            return koi::PolicyValueMctsEvaluation{
                0.0F, {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}};
        }
        const float remainder = 0.01F / static_cast<float>(legal.size() - 1);
        std::size_t preferred_index = legal.size();
        for (std::size_t index = 0; index < legal.size(); ++index) {
            if (legal[index].move == preferred) {
                preferred_index = index;
            }
            priors[index] = remainder;
        }
        if (preferred_index == legal.size()) {
            const float uniform = 1.0F / static_cast<float>(legal.size());
            std::fill(priors.begin(), priors.end(), uniform);
            return koi::PolicyValueMctsEvaluation{
                0.0F, {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}};
        }
        priors[preferred_index] = 0.99F;
        return koi::PolicyValueMctsEvaluation{
            0.0F, {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}};
    };

    koi::PolicyValueMctsConfig config;
    config.max_depth = 1;
    config.max_tree_nodes = 64;
    config.max_tree_edges = 512;
    koi::PolicyValueMctsTree first(root, moves, config, evaluator);
    require(first.initialize().has_value(), "MCTS root must initialize");
    std::atomic_bool stop_requested = false;
    const auto first_simulation = first.simulate(stop_requested);
    require(first_simulation.has_value(),
            first_simulation.has_value() ? "the first MCTS simulation must succeed" :
                                           first_simulation.error());
    require(first_simulation.value(), "the first MCTS simulation must visit a child");
    const auto first_result = first.snapshot(3);
    require(first_result.simulations == 1 && first_result.leaf_evaluations == 2,
            "one requested simulation must add exactly one leaf evaluation after the root");
    require(first_result.seldepth == 1,
            "the configured tree-ply limit must cap MCTS selection depth");
    require(!first_result.root_moves.empty() &&
                first_result.root_moves.front().move == *preferred &&
                first_result.root_moves.front().visits == 1,
            "PUCT must select the highest-prior root action deterministically");

    koi::PolicyValueMctsTree second(root, moves, config, evaluator);
    require(second.initialize().has_value(), "the repeated MCTS root must initialize");
    const auto second_simulation = second.simulate(stop_requested);
    require(second_simulation.has_value() && second_simulation.value(),
            second_simulation.has_value() ? "the repeated MCTS simulation must succeed" :
                                            second_simulation.error());
    const auto second_result = second.snapshot(3);
    require(first_result.root_moves.front().move == second_result.root_moves.front().move &&
                first_result.root_moves.front().visits == second_result.root_moves.front().visits,
            "single-thread MCTS tie-breaking and root selection must be deterministic");
}

void test_mcts_self_play_root_noise_is_normalized_and_seeded() {
    koi::GameState root = koi::GameState::startpos();
    const koi::MoveMetadataList moves = legal_root_moves(root);
    const auto priors_by_move = [](const koi::PolicyValueMctsSnapshot& snapshot) {
        std::map<std::string, double> priors;
        for (const auto& line : snapshot.root_moves) {
            priors[line.move.uci()] = line.prior;
        }
        return priors;
    };
    const auto prior_sum = [](const std::map<std::string, double>& priors) {
        double sum = 0.0;
        for (const auto& [move, prior] : priors) {
            (void)move;
            sum += prior;
        }
        return sum;
    };

    koi::PolicyValueMctsConfig normal_config;
    koi::PolicyValueMctsTree normal_tree(
        root, moves, normal_config, uniform_draw_evaluator());
    require(normal_tree.initialize().has_value(),
            "normal MCTS must initialize without self-play noise");
    const auto normal_priors = priors_by_move(normal_tree.snapshot(moves.size()));

    koi::PolicyValueMctsConfig noisy_config = normal_config;
    noisy_config.enable_root_noise = true;
    noisy_config.root_noise_seed = 0x12345678ULL;
    koi::PolicyValueMctsTree noisy_first(
        root, moves, noisy_config, uniform_draw_evaluator());
    koi::PolicyValueMctsTree noisy_second(
        root, moves, noisy_config, uniform_draw_evaluator());
    require(noisy_first.initialize().has_value() && noisy_second.initialize().has_value(),
            "self-play MCTS must initialize with seeded root noise");
    const auto first_priors = priors_by_move(noisy_first.snapshot(moves.size()));
    const auto repeated_priors = priors_by_move(noisy_second.snapshot(moves.size()));

    require(normal_priors.size() == moves.size() && first_priors.size() == moves.size(),
            "root snapshots must preserve the complete legal move distribution");
    require(std::abs(prior_sum(first_priors) - 1.0) < 1e-6,
            "Dirichlet root noise must preserve normalized priors");
    bool changed_from_normal = false;
    for (const auto& [move, prior] : first_priors) {
        require(std::abs(prior - repeated_priors.at(move)) < 1e-12,
                "the same self-play seed must reproduce root noise");
        if (std::abs(prior - normal_priors.at(move)) > 1e-5) {
            changed_from_normal = true;
        }
    }
    require(changed_from_normal,
            "self-play root noise must perturb the evaluator's uniform prior");

    noisy_config.root_noise_seed += 1;
    koi::PolicyValueMctsTree noisy_other_seed(
        root, moves, noisy_config, uniform_draw_evaluator());
    require(noisy_other_seed.initialize().has_value(),
            "a second self-play seed must initialize");
    const auto other_seed_priors = priors_by_move(noisy_other_seed.snapshot(moves.size()));
    bool changed_with_seed = false;
    for (const auto& [move, prior] : first_priors) {
        if (std::abs(prior - other_seed_priors.at(move)) > 1e-5) {
            changed_with_seed = true;
        }
    }
    require(changed_with_seed, "different self-play seeds must vary root noise");
}

void test_mcts_checkmate_leaf_backs_up_to_the_winning_side() {
    koi::GameState root = require_state("7k/5Q2/6K1/8/8/8/8/8 w - - 0 1");
    const auto mating_move = koi::Move::parse_uci("f7e8");
    require(mating_move.has_value() && root.is_legal(*mating_move),
            "the MCTS mate fixture must contain its reviewed legal move");
    const auto metadata = root.describe_move(*mating_move);
    require(metadata.has_value(), "the MCTS mating move metadata must be available");
    koi::GameState after_mate = root;
    require(after_mate.make_move(*mating_move) && after_mate.in_check() &&
                after_mate.legal_moves().empty(),
            "the selected MCTS child must be checkmate");
    koi::MoveMetadataList root_moves;
    require(root_moves.push_back(*metadata), "the MCTS root move must fit in fixed root storage");

    koi::PolicyValueMctsConfig config;
    config.max_depth = 1;
    config.max_tree_nodes = 8;
    config.max_tree_edges = 32;
    koi::PolicyValueMctsTree tree(root, root_moves, config, uniform_draw_evaluator());
    require(tree.initialize().has_value(), "the forced-mate MCTS root must initialize");
    std::atomic_bool stop_requested = false;
    require(tree.simulate(stop_requested).has_value(), "MCTS must expand the mating child");

    const auto result = tree.snapshot(1);
    require(result.root_moves.size() == 1 && result.root_moves[0].move == *mating_move,
            "MCTS must preserve the only legal root move");
    require(result.root_moves[0].value > 0.99F && result.value > 0.0F &&
                result.wdl[0] > result.wdl[2],
            "a checked node with no legal evasions must back up a win for the mover");
}

void test_mcts_tree_and_edge_capacity_are_bounded() {
    koi::GameState root = koi::GameState::startpos();
    const koi::MoveMetadataList moves = legal_root_moves(root);
    koi::PolicyValueMctsConfig config;
    config.max_tree_nodes = 1;
    config.max_tree_edges = koi::kMaximumLegalMoves;
    koi::PolicyValueMctsTree tree(root, moves, config, uniform_draw_evaluator());
    require(tree.initialize().has_value(), "the bounded MCTS root must initialize");
    std::atomic_bool stop_requested = false;
    for (int visit = 0; visit < 8; ++visit) {
        require(tree.simulate(stop_requested).has_value(),
                "a full tree must keep returning bounded leaf estimates");
    }
    const auto result = tree.snapshot(1);
    require(result.simulations == 8 && result.leaf_evaluations <= 1 + result.simulations &&
                result.tree_capped,
            "MCTS must respect the node cap while continuing to use bounded root edges");
}

void test_mcts_observes_cancellation_and_propagates_evaluator_errors() {
    koi::GameState root = koi::GameState::startpos();
    const koi::MoveMetadataList moves = legal_root_moves(root);
    koi::PolicyValueMctsConfig config;
    config.max_tree_nodes = 8;
    config.max_tree_edges = 64;
    koi::PolicyValueMctsTree tree(root, moves, config, uniform_draw_evaluator());
    require(tree.initialize().has_value(), "the cancellable MCTS root must initialize");
    std::atomic_bool stop_requested = true;
    const auto stopped = tree.simulate(stop_requested);
    require(stopped.has_value() && !stopped.value() && tree.snapshot(1).simulations == 0,
            "a stopped MCTS search must not begin another simulation");

    koi::PolicyValueMctsEvaluator failing = [](
        const koi::GameState&, const koi::MoveMetadataList&, const std::span<float>)
            -> std::expected<koi::PolicyValueMctsEvaluation, std::string> {
        return std::unexpected("synthetic policy/value inference failure");
    };
    koi::PolicyValueMctsTree failed_tree(root, moves, config, std::move(failing));
    const auto failure = failed_tree.initialize();
    require(!failure.has_value() &&
                failure.error() == "synthetic policy/value inference failure",
            "an inference error must leave MCTS initialization failed without publishing a move");
}

void test_mcts_deadline_interrupts_a_partial_simulation_and_recovers() {
    koi::GameState root = koi::GameState::startpos();
    const koi::MoveMetadataList moves = legal_root_moves(root);
    koi::PolicyValueMctsConfig config;
    config.max_depth = 8;
    config.max_tree_nodes = 128;
    config.max_tree_edges = 1'024;
    koi::PolicyValueMctsTree tree(root, moves, config, uniform_draw_evaluator());
    require(tree.initialize().has_value(), "the deadline-limited MCTS root must initialize");

    std::atomic_bool stop_requested = false;
    int deadline_checks = 0;
    const auto should_stop = [&deadline_checks] { return ++deadline_checks == 3; };
    const auto interrupted = tree.simulate(stop_requested, should_stop);
    require(interrupted.has_value() && !interrupted.value(),
            "a deadline raised during traversal must interrupt the partial visit");
    require(tree.simulations() == 0 && deadline_checks == 3,
            "an interrupted traversal must not back up a partial MCTS visit");

    const auto resumed = tree.simulate(stop_requested);
    require(resumed.has_value() && resumed.value() && tree.simulations() == 1,
            "a later visit must succeed after an interrupted path is unmade");
}

void test_mcts_claimable_root_draw_floors_losing_moves_but_keeps_wins() {
    koi::GameState root = require_state(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 100 51");
    require(root.is_claimable_draw(),
            "the root fixture must offer the fifty-move draw claim");
    const auto preferred = koi::Move::parse_uci("e2e4");
    require(preferred.has_value(), "the root draw fixture move must parse");
    const auto metadata = root.describe_move(*preferred);
    require(metadata.has_value(), "the root draw fixture move metadata must exist");
    koi::MoveMetadataList only_root_move;
    require(only_root_move.push_back(*metadata),
            "the restricted root move must fit the fixed buffer");

    const auto uniform = [](const koi::MoveMetadataList& legal,
                            const std::span<float> priors) {
        require(!legal.empty() && priors.size() == legal.size(),
                "the claimable-draw evaluator inputs must align");
        std::fill(priors.begin(), priors.end(), 1.0F / static_cast<float>(legal.size()));
    };
    const koi::PolicyValueMctsEvaluator losing_continuation =
        [uniform](const koi::GameState&, const koi::MoveMetadataList& legal,
                  const std::span<float> priors)
            -> std::expected<koi::PolicyValueMctsEvaluation, std::string> {
            uniform(legal, priors);
            return koi::PolicyValueMctsEvaluation{0.5F, {0.65F, 0.2F, 0.15F}};
        };
    koi::PolicyValueMctsConfig config;
    config.max_depth = 1;
    config.max_tree_nodes = 16;
    config.max_tree_edges = 64;
    koi::PolicyValueMctsTree losing_tree(
        root, only_root_move, config, losing_continuation);
    require(losing_tree.initialize().has_value(),
            "the claimable-draw MCTS root must initialize");
    std::atomic_bool stop_requested = false;
    for (int visit = 0; visit < 3; ++visit) {
        const auto simulated = losing_tree.simulate(stop_requested);
        require(simulated.has_value() && simulated.value(),
                "each losing continuation simulation must complete");
    }
    const auto losing_snapshot = losing_tree.snapshot(1);
    require(losing_snapshot.value == 0.0F &&
                losing_snapshot.wdl == std::array<float, 3>{0.0F, 1.0F, 0.0F},
            "the root aggregate must preserve the available draw claim");
    require(losing_snapshot.root_moves.size() == 1 &&
                losing_snapshot.root_moves[0].value >= 0.0F &&
                losing_snapshot.root_moves[0].wdl[1] > 0.99F,
            "a losing continuation must not outrank the available draw as a root score");

    const koi::PolicyValueMctsEvaluator winning_continuation =
        [uniform](const koi::GameState&, const koi::MoveMetadataList& legal,
                  const std::span<float> priors)
            -> std::expected<koi::PolicyValueMctsEvaluation, std::string> {
            uniform(legal, priors);
            return koi::PolicyValueMctsEvaluation{-0.5F, {0.15F, 0.2F, 0.65F}};
        };
    koi::PolicyValueMctsTree winning_tree(
        root, only_root_move, config, winning_continuation);
    require(winning_tree.initialize().has_value(),
            "the winning continuation tree must initialize");
    const auto winning_simulation = winning_tree.simulate(stop_requested);
    require(winning_simulation.has_value() && winning_simulation.value(),
            "the winning continuation must be evaluated");
    const auto winning_snapshot = winning_tree.snapshot(1);
    require(winning_snapshot.root_moves.size() == 1 &&
                winning_snapshot.root_moves[0].value > 0.4F &&
                winning_snapshot.root_moves[0].wdl[0] > 0.6F,
            "a winning continuation must remain above the draw-claim baseline");
}

} // namespace

int main(int argc, char** argv) {
    const std::array<koi::test::TestCase, 7> tests{{
        {"MCTS visits, depth and deterministic prior selection",
         test_mcts_visit_limit_depth_cap_and_deterministic_root_choice},
        {"MCTS self-play root noise", test_mcts_self_play_root_noise_is_normalized_and_seeded},
        {"MCTS checkmate WDL backup", test_mcts_checkmate_leaf_backs_up_to_the_winning_side},
        {"MCTS bounded tree capacity", test_mcts_tree_and_edge_capacity_are_bounded},
        {"MCTS stop and evaluator failure", test_mcts_observes_cancellation_and_propagates_evaluator_errors},
        {"MCTS deadline interruption and recovery",
         test_mcts_deadline_interrupts_a_partial_simulation_and_recovers},
        {"MCTS claimable root draw ranking",
         test_mcts_claimable_root_draw_floors_losing_moves_but_keeps_wins},
    }};
    return koi::test::run_tests(tests, argc, argv);
}
