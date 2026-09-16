#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "koi/game_state.hpp"
#include "koi/position.hpp"

namespace koi::detail {

inline constexpr std::size_t kMaximumGameStateHistory = 256;

using FeatureBuilder = PositionFeatures (*)(const Position&) noexcept;

class FeatureState final {
public:
    FeatureState();
    FeatureState(const FeatureState& other);
    FeatureState(const FeatureState& other, std::size_t source_history_size);

    [[nodiscard]] PositionFeatures get_or_compute(
        std::size_t cache_index, std::uint64_t position_key,
        const Position& position, FeatureBuilder builder) const noexcept;
    void invalidate(std::size_t cache_index) noexcept;
    [[nodiscard]] std::uint64_t cache_misses() const noexcept;

private:
    struct Entry {
        std::uint64_t position_key = 0;
        PositionFeatures features{};
    };

    mutable std::shared_mutex mutex_;
    mutable std::array<std::unique_ptr<Entry>, kMaximumGameStateHistory> slots_{};
    mutable std::array<bool, kMaximumGameStateHistory> valid_{};
    mutable std::array<std::atomic<Entry*>, kMaximumGameStateHistory> published_{};
    mutable std::array<std::atomic_bool, kMaximumGameStateHistory> published_valid_{};
    mutable std::array<std::atomic_uint64_t, kMaximumGameStateHistory> keys_{};
    mutable std::uint64_t cache_misses_ = 0;
};

} // namespace koi::detail
