// GPU NNUE probe: verifies the nvcc -> embedded PTX -> CUDA driver path and
// runs the v5 evaluation kernel once with zero weights, where the expected
// score is exactly zero.  Built CPU-only when nvcc is unavailable.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "koi/gpu/cuda_driver.hpp"

#if KOI_GPU_INFERENCE_AVAILABLE
#include "koi_nnue_v5_ptx.hpp"
#endif

namespace {

constexpr std::size_t kInput = 36864;
constexpr std::size_t kHidden = 1536;
constexpr std::size_t kL1 = 32;
constexpr std::size_t kBuckets = 8;

struct Position {
    std::uint64_t occupancy[2] = {0, 0};
    std::uint8_t pieces[64] = {};
    std::uint8_t side_to_move = 0;
    std::uint8_t king_squares[2] = {0, 0};
};

Position startpos() {
    Position position;
    const std::uint8_t back_rank[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (std::uint8_t file = 0; file < 8; ++file) {
        position.pieces[file] = back_rank[file];
        position.pieces[8 + file] = 1;
        position.pieces[48 + file] = static_cast<std::uint8_t>(1U | (1U << 3));
        position.pieces[56 + file] = static_cast<std::uint8_t>(back_rank[file] | (1U << 3));
    }
    position.occupancy[0] = 0x000000000000FFFFULL;
    position.occupancy[1] = 0xFFFF000000000000ULL;
    position.side_to_move = 0;
    position.king_squares[0] = 4;  // e1
    position.king_squares[1] = 60; // e8
    return position;
}

} // namespace

int main() {
#if !KOI_GPU_INFERENCE_AVAILABLE
    std::puts("gpu-probe: built without nvcc; GPU inference is unavailable");
    return 0;
#else
    std::string error;
    koi::gpu::CudaDriver driver = koi::gpu::CudaDriver::open(error);
    if (!driver.available()) {
        std::printf("gpu-probe: CUDA driver unavailable: %s\n",
                    error.empty() ? "unknown reason" : error.c_str());
        return 2;
    }
    const koi::gpu::CudaDeviceInfo& info = driver.device_info();
    std::printf("gpu-probe: device %s (sm_%d%d), driver %d\n",
                info.name.c_str(), info.compute_major, info.compute_minor,
                info.driver_version);

    const auto ptx = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(koi::gpu::kNnueV5PtxSource),
        std::char_traits<char>::length(koi::gpu::kNnueV5PtxSource));
    if (!driver.load_module(ptx, error)) {
        std::printf("gpu-probe: module load failed: %s\n", error.c_str());
        return 3;
    }
    CUfunction function = nullptr;
    if (!driver.module_function("koi_nnue_v5_eval", function, error)) {
        std::printf("gpu-probe: kernel lookup failed: %s\n", error.c_str());
        return 3;
    }

    const std::vector<Position> positions(1, startpos());
    std::vector<std::int16_t> feature_weights(kInput * kHidden, 0);
    std::vector<std::int32_t> hidden_bias(kHidden, 0);
    std::vector<std::int8_t> l1_weights(kL1 * kHidden, 0);
    std::vector<std::int32_t> l1_bias(kL1, 0);
    std::vector<std::int8_t> output_weights(kBuckets * kL1, 0);
    std::vector<std::int32_t> output_bias(kBuckets, 0);
    std::vector<std::int32_t> scores(positions.size(), -12345);
    std::vector<std::uint8_t> overflow(positions.size(), 0);

    struct Buffer {
        CUdeviceptr pointer = 0;
        std::size_t bytes = 0;
    };
    std::vector<Buffer> device_buffers;
    const auto allocate = [&](std::size_t bytes, CUdeviceptr& pointer) {
        if (!driver.allocate(bytes, pointer, error)) {
            std::printf("gpu-probe: allocation failed: %s\n", error.c_str());
            return false;
        }
        device_buffers.push_back(Buffer{pointer, bytes});
        return true;
    };
    const auto stage = [&](const void* source, std::size_t bytes, CUdeviceptr& pointer) {
        return allocate(bytes, pointer) && driver.upload(source, pointer, bytes, error);
    };

    CUdeviceptr occupancy = 0;
    CUdeviceptr pieces = 0;
    CUdeviceptr side = 0;
    CUdeviceptr kings = 0;
    CUdeviceptr w1 = 0;
    CUdeviceptr b1 = 0;
    CUdeviceptr w1h = 0;
    CUdeviceptr b1h = 0;
    CUdeviceptr w2o = 0;
    CUdeviceptr b2o = 0;
    CUdeviceptr score_buffer = 0;
    CUdeviceptr overflow_buffer = 0;
    bool staged = true;
    staged &= stage(positions.data()->occupancy, sizeof(Position) * positions.size(), occupancy);
    staged &= stage(positions.data()->pieces, sizeof(Position) * positions.size(), pieces);
    staged &= stage(&positions.data()->side_to_move, sizeof(Position) * positions.size(), side);
    staged &= stage(positions.data()->king_squares, sizeof(Position) * positions.size(), kings);
    staged &= stage(feature_weights.data(), feature_weights.size() * sizeof(std::int16_t), w1);
    staged &= stage(hidden_bias.data(), hidden_bias.size() * sizeof(std::int32_t), b1);
    staged &= stage(l1_weights.data(), l1_weights.size() * sizeof(std::int8_t), w1h);
    staged &= stage(l1_bias.data(), l1_bias.size() * sizeof(std::int32_t), b1h);
    staged &= stage(output_weights.data(), output_weights.size() * sizeof(std::int8_t), w2o);
    staged &= stage(output_bias.data(), output_bias.size() * sizeof(std::int32_t), b2o);
    staged &= allocate(scores.size() * sizeof(std::int32_t), score_buffer);
    staged &= allocate(overflow.size() * sizeof(std::uint8_t), overflow_buffer);
    if (!staged) {
        std::printf("gpu-probe: staging failed: %s\n", error.c_str());
        for (const Buffer& buffer : device_buffers) {
            driver.release(buffer.pointer);
        }
        return 3;
    }

    const int l1_shift = 0;
    const int output_shift = 0;
    void* arguments[] = {
        &occupancy, &pieces, &side, &kings, &w1, &b1, &w1h, &b1h, &w2o, &b2o,
        const_cast<int*>(&l1_shift), const_cast<int*>(&output_shift),
        &score_buffer, &overflow_buffer,
    };
    if (!driver.launch_and_wait(function, static_cast<unsigned>(positions.size()), 256, 0,
                                arguments, error)) {
        std::printf("gpu-probe: kernel launch failed: %s\n", error.c_str());
        return 3;
    }
    if (!driver.download(score_buffer, scores.data(),
                         scores.size() * sizeof(std::int32_t), error) ||
        !driver.download(overflow_buffer, overflow.data(),
                         overflow.size() * sizeof(std::uint8_t), error)) {
        std::printf("gpu-probe: download failed: %s\n", error.c_str());
        return 3;
    }
    std::printf("gpu-probe: score %d overflow %u\n", scores[0], overflow[0]);
    const bool healthy = scores[0] == 0 && overflow[0] == 0;
    for (const Buffer& buffer : device_buffers) {
        driver.release(buffer.pointer);
    }
    if (!healthy) {
        std::puts("gpu-probe: unexpected zero-weight result");
        return 3;
    }
    std::puts("gpu-probe: OK");
    return 0;
#endif
}
