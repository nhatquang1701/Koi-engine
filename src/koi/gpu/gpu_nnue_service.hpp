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

// The service stages this many positions per launch.  The batching evaluator
// must never hand it a larger batch: a bigger one fails and falls back to the
// CPU, so the batch size is clamped to this limit.
inline constexpr std::size_t kMaximumGpuBatchSize = 256;

// Synchronous GPU evaluation service for the v5 network.  The service owns the
// device weights and a small amount of staging memory; callers pass a batch of
// positions and receive one score per position.  Every failure returns false so
// the caller can fall back to the CPU implementation.
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

private:
    GpuNnueService();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi::gpu
