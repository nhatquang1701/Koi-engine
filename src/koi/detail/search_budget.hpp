#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "koi/time_manager.hpp"

namespace koi::detail {

class SearchBudget final {
public:
    explicit SearchBudget(const TimeManager& time_manager,
                          std::atomic<std::uint64_t>* shared_nodes = nullptr) noexcept
        : limit_(time_manager.node_limit()), shared_nodes_(shared_nodes) {}

    [[nodiscard]] bool reserve(const std::uint64_t local_nodes) noexcept {
        if (shared_nodes_ == nullptr) {
            return !limit_.has_value() || local_nodes < *limit_;
        }

        if (!limit_.has_value()) {
            shared_nodes_->fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        std::uint64_t observed = shared_nodes_->load(std::memory_order_relaxed);
        for (;;) {
            if (observed >= *limit_) {
                return false;
            }
            if (shared_nodes_->compare_exchange_weak(
                    observed, observed + 1, std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] std::uint64_t visited(const std::uint64_t local_nodes) const noexcept {
        if (shared_nodes_ != nullptr) {
            return shared_nodes_->load(std::memory_order_relaxed);
        }
        return local_nodes;
    }

    [[nodiscard]] bool uses_shared_counter() const noexcept {
        return shared_nodes_ != nullptr;
    }

private:
    std::optional<std::uint64_t> limit_;
    std::atomic<std::uint64_t>* shared_nodes_;
};

} // namespace koi::detail
