#pragma once

#include <cstdint>
#include <optional>

#include "koi/transposition_table.hpp"

namespace koi::detail {

class SearchTableAccess final {
public:
    explicit SearchTableAccess(TranspositionTable& table, const bool enabled = true) noexcept
        : table_(&table), enabled_(enabled) {}

    void set_enabled(const bool enabled) noexcept {
        enabled_ = enabled;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_;
    }

    [[nodiscard]] std::optional<TranspositionEntry> probe(
        const std::uint64_t key, const int ply = 0) const noexcept {
        if (!enabled_) {
            return std::nullopt;
        }
        return table_->probe(key, ply);
    }

    void store(const std::uint64_t key, const int depth, const int score,
               const TranspositionBound bound, const Move best_move,
               const int ply = 0) noexcept {
        if (enabled_) {
            table_->store(key, depth, score, bound, best_move, ply);
        }
    }

private:
    TranspositionTable* table_;
    bool enabled_;
};

} // namespace koi::detail
