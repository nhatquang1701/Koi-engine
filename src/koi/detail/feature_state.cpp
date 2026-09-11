#include "koi/detail/feature_state.hpp"

#include <algorithm>

namespace koi::detail {

FeatureState::FeatureState() {
    for (std::size_t index = 0; index < kMaximumGameStateHistory; ++index) {
        published_[index].store(nullptr, std::memory_order_relaxed);
        published_valid_[index].store(false, std::memory_order_relaxed);
        keys_[index].store(0, std::memory_order_relaxed);
    }
}

FeatureState::FeatureState(const FeatureState& other)
    : FeatureState(other, kMaximumGameStateHistory - 1) {}

FeatureState::FeatureState(const FeatureState& other,
                           const std::size_t source_history_size) {
    for (std::size_t index = 0; index < kMaximumGameStateHistory; ++index) {
        published_[index].store(nullptr, std::memory_order_relaxed);
        published_valid_[index].store(false, std::memory_order_relaxed);
        keys_[index].store(0, std::memory_order_relaxed);
    }

    std::shared_lock lock(other.mutex_);
    const std::size_t copy_count = std::min(
        source_history_size + 1, kMaximumGameStateHistory);
    for (std::size_t index = 0; index < copy_count; ++index) {
        if (other.valid_[index] && other.slots_[index] != nullptr) {
            slots_[index] = std::make_unique<Entry>(*other.slots_[index]);
        }
        valid_[index] = other.valid_[index];
        const std::uint64_t key = valid_[index] ?
            other.keys_[index].load(std::memory_order_acquire) : 0;
        if (valid_[index] && slots_[index] != nullptr) {
            slots_[index]->position_key = key;
            published_[index].store(slots_[index].get(), std::memory_order_release);
            keys_[index].store(key, std::memory_order_release);
            published_valid_[index].store(true, std::memory_order_release);
        }
    }
    cache_misses_ = other.cache_misses_;
    snapshot_copies_.store(
        other.snapshot_copies_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    fast_hits_.store(other.fast_hits_.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
}

PositionFeatures FeatureState::get_or_compute(const std::size_t cache_index,
                                              const std::uint64_t position_key,
                                              const Position& position,
                                              const FeatureBuilder builder) const noexcept {
    if (builder == nullptr) {
        return {};
    }

    if (cache_index < kMaximumGameStateHistory &&
        published_valid_[cache_index].load(std::memory_order_acquire)) {
        const Entry* entry = published_[cache_index].load(std::memory_order_acquire);
        if (entry != nullptr && keys_[cache_index].load(std::memory_order_acquire) == position_key) {
            ++fast_hits_;
            return entry->features;
        }
    }

    std::unique_lock lock(mutex_);
    if (cache_index < kMaximumGameStateHistory) {
        const auto& entry = slots_[cache_index];
        if (valid_[cache_index] && entry != nullptr && entry->position_key == position_key) {
            return entry->features;
        }
    }

    ++cache_misses_;
    const PositionFeatures features = builder(position);
    if (cache_index < kMaximumGameStateHistory) {
        try {
            if (slots_[cache_index] == nullptr) {
                slots_[cache_index] = std::make_unique<Entry>();
            }
            slots_[cache_index]->position_key = position_key;
            slots_[cache_index]->features = features;
            valid_[cache_index] = true;
            published_[cache_index].store(slots_[cache_index].get(), std::memory_order_release);
            keys_[cache_index].store(position_key, std::memory_order_release);
            published_valid_[cache_index].store(true, std::memory_order_release);
        } catch (...) {
            valid_[cache_index] = false;
            published_valid_[cache_index].store(false, std::memory_order_release);
            keys_[cache_index].store(0, std::memory_order_release);
        }
    }
    return features;
}

void FeatureState::invalidate(const std::size_t cache_index) noexcept {
    std::unique_lock lock(mutex_);
    if (cache_index >= kMaximumGameStateHistory) {
        return;
    }
    valid_[cache_index] = false;
    published_valid_[cache_index].store(false, std::memory_order_release);
    keys_[cache_index].store(0, std::memory_order_release);
}

std::uint64_t FeatureState::cache_misses() const noexcept {
    std::shared_lock lock(mutex_);
    return cache_misses_;
}

std::uint64_t FeatureState::fast_hits() const noexcept {
    return fast_hits_.load(std::memory_order_relaxed);
}

std::uint64_t FeatureState::snapshot_copies() const noexcept {
    return snapshot_copies_.load(std::memory_order_relaxed);
}

} // namespace koi::detail
