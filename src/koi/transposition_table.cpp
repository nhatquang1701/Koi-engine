#include "koi/transposition_table.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace koi {
namespace {

constexpr int kMateThreshold = 99'000;

int score_for_storage(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score + ply;
    }
    if (score <= -kMateThreshold) {
        return score - ply;
    }
    return score;
}

int score_for_probe(int score, int ply) noexcept {
    if (score >= kMateThreshold) {
        return score - ply;
    }
    if (score <= -kMateThreshold) {
        return score + ply;
    }
    return score;
}

} // namespace

struct TranspositionTable::Storage {
    explicit Storage(std::size_t megabytes)
        : entries(std::max<std::size_t>(1, (megabytes * 1024ULL * 1024ULL) /
                                             sizeof(TranspositionEntry))),
          size_mb(megabytes) {}

    std::vector<TranspositionEntry> entries;
    std::array<std::shared_mutex, kStripeCount> stripes;
    const std::size_t size_mb;
    std::uint16_t generation = 1;
};

TranspositionTable::TranspositionTable(std::size_t megabytes)
    : storage_(std::make_shared<Storage>(normalized_size_mb(megabytes))) {}

std::shared_ptr<TranspositionTable::Storage> TranspositionTable::snapshot() const noexcept {
    return std::atomic_load_explicit(&storage_, std::memory_order_acquire);
}

std::size_t TranspositionTable::normalized_size_mb(std::size_t megabytes) noexcept {
    return std::clamp(megabytes, std::size_t{1}, std::size_t{4096});
}

void TranspositionTable::set_size_mb(std::size_t megabytes) {
    const std::size_t normalized = normalized_size_mb(megabytes);
    auto replacement = std::make_shared<Storage>(normalized);
    std::lock_guard lock(maintenance_mutex_);
    std::atomic_store_explicit(&storage_, std::move(replacement), std::memory_order_release);
}

std::size_t TranspositionTable::size_mb() const noexcept {
    const auto storage = snapshot();
    return storage == nullptr ? 0 : storage->size_mb;
}

void TranspositionTable::clear() noexcept {
    std::lock_guard maintenance_lock(maintenance_mutex_);
    const auto storage = snapshot();
    if (storage == nullptr) {
        return;
    }

    std::array<std::unique_lock<std::shared_mutex>, kStripeCount> stripe_locks;
    for (std::size_t index = 0; index < kStripeCount; ++index) {
        stripe_locks[index] = std::unique_lock<std::shared_mutex>(storage->stripes[index]);
    }
    std::fill(storage->entries.begin(), storage->entries.end(), TranspositionEntry{});
}

void TranspositionTable::new_generation() noexcept {
    std::lock_guard maintenance_lock(maintenance_mutex_);
    const auto storage = snapshot();
    if (storage == nullptr) {
        return;
    }

    std::array<std::unique_lock<std::shared_mutex>, kStripeCount> stripe_locks;
    for (std::size_t index = 0; index < kStripeCount; ++index) {
        stripe_locks[index] = std::unique_lock<std::shared_mutex>(storage->stripes[index]);
    }
    ++storage->generation;
    if (storage->generation == 0) {
        storage->generation = 1;
        std::fill(storage->entries.begin(), storage->entries.end(), TranspositionEntry{});
    }
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move,
                               int ply) noexcept {
    const auto storage = snapshot();
    if (storage == nullptr || storage->entries.empty()) {
        return;
    }

    const std::size_t index = key % storage->entries.size();
    std::unique_lock stripe_lock(storage->stripes[index % kStripeCount]);
    TranspositionEntry& existing = storage->entries[index];
    const bool empty = !existing.occupied;
    const bool same_key = existing.key == key;
    if (!empty && same_key) {
        const bool keeps_deeper_entry = existing.depth > depth;
        const bool keeps_equal_exact_entry = existing.depth == depth &&
            existing.bound == TranspositionBound::exact && bound != TranspositionBound::exact;
        if (keeps_deeper_entry || keeps_equal_exact_entry) {
            existing.generation = storage->generation;
            return;
        }
    }
    const bool older_generation = existing.generation != storage->generation;
    const bool deeper = depth > existing.depth;
    const bool deterministic_tie_break = depth == existing.depth && key < existing.key;
    if (!empty && !same_key && !older_generation && !deeper && !deterministic_tie_break) {
        return;
    }

    existing = TranspositionEntry{key, depth, score_for_storage(score, ply), bound, best_move,
                                  storage->generation, true};
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key, int ply) const noexcept {
    const auto storage = snapshot();
    if (storage == nullptr || storage->entries.empty()) {
        return std::nullopt;
    }

    const std::size_t index = key % storage->entries.size();
    std::shared_lock stripe_lock(storage->stripes[index % kStripeCount]);
    const TranspositionEntry& entry = storage->entries[index];
    if (!entry.occupied || entry.key != key) {
        return std::nullopt;
    }
    TranspositionEntry result = entry;
    result.score = score_for_probe(result.score, ply);
    return result;
}

} // namespace koi
