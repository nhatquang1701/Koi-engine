#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace koi {
class GameState;
struct NnueNetwork;
} // namespace koi

namespace koi::gpu {

// Positions staged per kernel launch.  This single internal constant is both
// the service's staging capacity and the batching evaluator's cap, so the
// batcher can never hand the service a larger batch than it can hold (a bigger
// one would fail and fall back to the CPU).  Retune the cap here; the
// KOI_GPU_BATCH environment variable is a test seam that can only lower the
// batcher's request size within it.
inline constexpr std::size_t kMaximumGpuBatchSize = 256;

// Synchronous GPU evaluation service for the v5 network.  The service owns the
// device weights and a small amount of staging memory; callers pass a batch of
// positions and receive one score per position.  Every failure returns false so
// the caller can fall back to the CPU implementation.  Version 6 containers
// have a different L1 stage and are rejected explicitly (no v6 kernel exists).
class GpuNnueService {
public:
    ~GpuNnueService();
    GpuNnueService(const GpuNnueService&) = delete;
    GpuNnueService& operator=(const GpuNnueService&) = delete;
    GpuNnueService(GpuNnueService&&) noexcept;
    GpuNnueService& operator=(GpuNnueService&&) noexcept;

    // Returns null when the driver, the device, or the network are unusable.
    [[nodiscard]] static std::unique_ptr<GpuNnueService> create(
        const NnueNetwork& network, std::string& error);

    // Evaluates every position from the side to move's perspective.  The score
    // is the exact CPU scalar result for the same weights, or the call fails.
    [[nodiscard]] bool evaluate(std::span<const GameState*> states,
                                std::span<std::int32_t> scores, std::string& error);

    [[nodiscard]] bool available() const noexcept;
    // Name of the selected CUDA device, or an empty string when no driver was
    // opened.  Used by the UCI diagnostic ("... -> cuda (GTX 1060)").
    [[nodiscard]] const std::string& device_name() const noexcept;

private:
    GpuNnueService();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi::gpu
