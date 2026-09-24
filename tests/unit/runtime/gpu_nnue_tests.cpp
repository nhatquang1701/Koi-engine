// GPU NNUE parity tests: the kernel must reproduce the CPU scalar evaluation
// exactly.  Without a CUDA driver (or in a build without nvcc) the cases skip.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "koi/classical_evaluator.hpp"
#include "koi/game_state.hpp"
#include "koi/gpu/gpu_nnue_service.hpp"
#include "koi/gpu/nnue_gpu_evaluator.hpp"
#include "koi/gpu/ptx_variant.hpp"
#include "koi/nnue.hpp"
#include "koi_test_support.hpp"

using koi::test::require;
using koi::test::skip;

namespace {

constexpr const char* kFixtures[] = {
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
    "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
};

std::shared_ptr<const koi::NnueNetwork> test_network() {
    constexpr std::size_t kHidden = 1536;
    constexpr std::size_t kL1 = 32;
    koi::NnueNetwork network = koi::NnueNetwork::synthetic_v5();
    network.manifest.layer_sizes = {36864, kHidden, 8, kL1};
    network.feature_weights.assign(36864 * kHidden, 0);
    for (std::size_t index = 0; index < network.feature_weights.size(); ++index) {
        network.feature_weights[index] =
            static_cast<std::int16_t>(static_cast<std::int32_t>(index * 37 % 2001) - 1000);
    }
    network.hidden_bias.assign(kHidden, 0);
    for (std::size_t index = 0; index < kHidden; ++index) {
        network.hidden_bias[index] = static_cast<std::int32_t>(index % 251) - 125;
    }
    network.l1_weights.assign(kL1 * kHidden, 0);
    for (std::size_t index = 0; index < network.l1_weights.size(); ++index) {
        network.l1_weights[index] =
            static_cast<std::int8_t>(static_cast<std::int32_t>(index * 29 % 255) - 127);
    }
    network.l1_bias.assign(kL1, 0);
    for (std::size_t index = 0; index < kL1; ++index) {
        network.l1_bias[index] = static_cast<std::int32_t>(index * 13) - 50;
    }
    network.bottleneck_weights.assign(8 * kL1, 0);
    for (std::size_t index = 0; index < network.bottleneck_weights.size(); ++index) {
        network.bottleneck_weights[index] =
            static_cast<std::int8_t>(static_cast<std::int32_t>(index * 43 % 255) - 127);
    }
    network.bottleneck_bias.assign(8, 0);
    for (std::size_t index = 0; index < 8; ++index) {
        network.bottleneck_bias[index] = static_cast<std::int32_t>(index * 17) - 60;
    }
    network.l1_shift = 6;
    network.output_shift = 12;
    return std::make_shared<const koi::NnueNetwork>(std::move(network));
}

std::vector<koi::GameState> fixture_states() {
    std::vector<koi::GameState> states;
    for (const char* fen : kFixtures) {
        auto parsed = koi::GameState::from_fen(fen);
        require(parsed.has_value(), std::string("GPU fixture must parse: ") + fen);
        states.push_back(*parsed);
    }
    return states;
}

std::vector<std::int32_t> cpu_scores(const std::shared_ptr<const koi::NnueNetwork>& weights,
                                     const std::vector<koi::GameState>& states) {
    koi::NnueWorker worker(weights);
    std::vector<std::int32_t> scores(states.size(), 0);
    for (std::size_t index = 0; index < states.size(); ++index) {
        scores[index] = worker.evaluate(states[index], states[index].side_to_move(),
                                        koi::NnueInferencePath::scalar);
    }
    return scores;
}

std::unique_ptr<koi::gpu::GpuNnueService> open_service(
    const std::shared_ptr<const koi::NnueNetwork>& weights, std::string& error) {
    return koi::gpu::GpuNnueService::create(*weights, error);
}

void test_gpu_matches_cpu_scalar() {
#if !KOI_GPU_INFERENCE_AVAILABLE
    skip("this build has no GPU NNUE kernel");
#else
    const auto weights = test_network();
    std::string error;
    std::unique_ptr<koi::gpu::GpuNnueService> service = open_service(weights, error);
    if (!service) {
        skip("GPU NNUE service is unavailable: " + error);
    }
    std::vector<koi::GameState> states = fixture_states();
    std::vector<const koi::GameState*> pointers;
    pointers.reserve(states.size());
    for (const koi::GameState& state : states) {
        pointers.push_back(&state);
    }
    std::vector<std::int32_t> gpu_scores(states.size(), 0);
    require(service->evaluate(pointers, gpu_scores, error),
            "GPU batch evaluation must succeed: " + error);
    const std::vector<std::int32_t> expected = cpu_scores(weights, states);
    for (std::size_t index = 0; index < states.size(); ++index) {
        require(gpu_scores[index] == expected[index],
                "GPU score must match the CPU scalar score: " +
                    std::to_string(gpu_scores[index]) + " != " +
                    std::to_string(expected[index]));
    }
#endif
}

void test_gpu_matches_cpu_across_batch_sizes() {
#if !KOI_GPU_INFERENCE_AVAILABLE
    skip("this build has no GPU NNUE kernel");
#else
    const auto weights = test_network();
    std::string error;
    std::unique_ptr<koi::gpu::GpuNnueService> service = open_service(weights, error);
    if (!service) {
        skip("GPU NNUE service is unavailable: " + error);
    }
    std::vector<koi::GameState> states;
    for (const char* fen : kFixtures) {
        auto parsed = koi::GameState::from_fen(fen);
        require(parsed.has_value(), std::string("GPU batch fixture must parse: ") + fen);
        states.push_back(*parsed);
    }
    while (states.size() < 256) {
        const koi::GameState copy = states[states.size() % 5];
        states.push_back(copy);
    }
    const std::vector<std::int32_t> expected = cpu_scores(weights, states);
    for (const std::size_t batch : {1U, 2U, 3U, 64U, 256U}) {
        std::vector<const koi::GameState*> pointers;
        pointers.reserve(batch);
        for (std::size_t index = 0; index < batch; ++index) {
            pointers.push_back(&states[index]);
        }
        std::vector<std::int32_t> gpu_scores(batch, 0);
        std::string error;
        require(service->evaluate(pointers, gpu_scores, error),
                "GPU batch evaluation must succeed for batch size " +
                    std::to_string(batch) + ": " + error);
        for (std::size_t index = 0; index < batch; ++index) {
            require(gpu_scores[index] == expected[index],
                    "GPU batch score must match the CPU scalar score at size " +
                        std::to_string(batch) + ": " +
                        std::to_string(gpu_scores[index]) + " != " +
                        std::to_string(expected[index]));
        }
    }
#endif
}

void test_concurrent_batch_requests_keep_their_own_scores() {
#if !KOI_GPU_INFERENCE_AVAILABLE
    skip("this build has no GPU NNUE kernel");
#else
    const auto weights = test_network();
    std::string error;
    std::unique_ptr<koi::gpu::GpuNnueService> service = open_service(weights, error);
    if (!service) {
        skip("GPU NNUE service is unavailable: " + error);
    }
    std::vector<koi::GameState> states = fixture_states();
    koi::gpu::begin_gpu_nnue_threaded_search();
    koi::gpu::GpuNnueEvaluator evaluator(std::make_shared<koi::ClassicalEvaluator>(),
                                         std::move(service));

    // Sequential reference through the same batcher.
    std::vector<int> reference(states.size(), 0);
    for (std::size_t index = 0; index < states.size(); ++index) {
        require(evaluator.evaluate_on_gpu(states[index], states[index].side_to_move(),
                                          reference[index]),
                "the GPU must evaluate the fixture positions");
    }

    // Eight threads issue overlapping requests: a finished batch must not free a
    // request while its owner is still reading the result.
    constexpr int kRounds = 8;
    std::vector<std::thread> threads;
    std::vector<int> observed(states.size() * kRounds, 0);
    std::atomic<bool> failed{false};
    for (int round = 0; round < kRounds; ++round) {
        threads.emplace_back([&, round] {
            for (std::size_t index = 0; index < states.size(); ++index) {
                int score = 0;
                if (!evaluator.evaluate_on_gpu(states[index], states[index].side_to_move(),
                                               score)) {
                    failed.store(true);
                    return;
                }
                observed[static_cast<std::size_t>(round) * states.size() + index] = score;
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    koi::gpu::end_gpu_nnue_threaded_search();
    require(!failed.load(), "every concurrent GPU request must succeed");
    for (int round = 0; round < kRounds; ++round) {
        for (std::size_t index = 0; index < states.size(); ++index) {
            require(observed[static_cast<std::size_t>(round) * states.size() + index] ==
                        reference[index],
                    "a concurrent request must observe the score for its own position");
        }
    }
#endif
}

void test_ptx_variant_selection_prefers_the_newest_supported_module() {
    // PTX JITs forward only, so a device runs the newest embedded module whose
    // compute capability does not exceed its own.  This case needs no GPU and
    // runs in every build, including the CPU-only ones.
    constexpr std::array<koi::gpu::NnueV5PtxVariant, 5> variants{{
        {6, 1, "sm61"},
        {7, 5, "sm75"},
        {8, 6, "sm86"},
        {8, 9, "sm89"},
        {12, 0, "sm120"},
    }};
    koi::test::require(koi::gpu::select_ptx_variant(variants, 6, 1) == 0,
                       "sm_61 must select the Pascal module");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 7, 5) == 1,
                       "sm_75 must select the Turing module");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 8, 0) == 1,
                       "sm_80 must JIT the newest module at or below it");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 8, 6) == 2,
                       "sm_86 must select the Ampere module");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 8, 9) == 3,
                       "sm_89 must select the Ada module");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 9, 0) == 3,
                       "sm_90 must JIT the newest module at or below it");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 12, 0) == 4,
                       "sm_120 must select the Blackwell module");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 12, 1) == 4,
                       "a newer minor must still use the newest module at or below it");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 5, 2) == -1,
                       "a device older than every module must fall back to the CPU");
    koi::test::require(koi::gpu::select_ptx_variant(variants, 6, 0) == -1,
                       "a device just below the oldest module must fall back to the CPU");
}

void test_ptx_candidate_list_orders_loadable_modules_newest_first() {
    // PTX JIT is forward-compatible but not backward-compatible, and a driver
    // can reject a module it cannot JIT.  The candidate list lets the service
    // fall back to older modules, so it must be ordered newest first and stop
    // at the device ceiling.
    constexpr std::array<koi::gpu::NnueV5PtxVariant, 5> variants{{
        {6, 1, "sm61"},
        {7, 5, "sm75"},
        {8, 6, "sm86"},
        {8, 9, "sm89"},
        {12, 0, "sm120"},
    }};
    std::array<int, 5> candidates{};
    koi::test::require(koi::gpu::select_ptx_candidates(variants, 12, 0, candidates) == 5,
                       "sm_120 must offer all five modules");
    koi::test::require(candidates[0] == 4 && candidates[1] == 3 && candidates[2] == 2 &&
                           candidates[3] == 1 && candidates[4] == 0,
                       "candidates must be ordered newest first");
    koi::test::require(koi::gpu::select_ptx_candidates(variants, 8, 0, candidates) == 2,
                       "sm_80 must offer the two modules it can run");
    koi::test::require(candidates[0] == 1 && candidates[1] == 0,
                       "sm_80 must offer sm_75 before sm_61");
    koi::test::require(koi::gpu::select_ptx_candidates(variants, 6, 1, candidates) == 1,
                       "sm_61 must offer exactly the Pascal module");
    koi::test::require(koi::gpu::select_ptx_candidates(variants, 5, 2, candidates) == 0,
                       "a device older than every module must offer no candidates");
}

} // namespace

int main(int argc, char** argv) {
    const std::array<koi::test::TestCase, 5> tests{{
        {"GPU NNUE matches CPU scalar", test_gpu_matches_cpu_scalar},
        {"GPU NNUE batch sizes agree", test_gpu_matches_cpu_across_batch_sizes},
        {"GPU NNUE concurrent requests", test_concurrent_batch_requests_keep_their_own_scores},
        {"GPU NNUE PTX variant selection", test_ptx_variant_selection_prefers_the_newest_supported_module},
        {"GPU NNUE PTX candidate list", test_ptx_candidate_list_orders_loadable_modules_newest_first},
    }};
    return koi::test::run_tests(tests, argc, argv);
}
