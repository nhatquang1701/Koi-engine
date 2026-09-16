#include "koi/transposition_table.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace koi {
namespace {

constexpr int kMateThreshold = 99'000;
constexpr std::size_t kBytesPerMegabyte = 1024ULL * 1024ULL;
constexpr std::size_t kSegmentBytes = 32ULL * kBytesPerMegabyte;
constexpr std::size_t kMinimumMegabytes = 1;
constexpr std::size_t kMaximumMegabytes = 4096;
// Keep several entries in each replacement bucket.  The search key includes
// rule-history context, so unrelated reversible paths collide more often than
// a board-only TT would.  A small cluster preserves those useful bounds while
// keeping the probe/store lock and memory layout cache-friendly.
constexpr std::size_t kClusterSize = 4;
// Number of slots inspected by TranspositionTable::hashfull_permill().  The
// UCI value is permill, so 1000 slots is the natural resolution and keeps the
// sampling cost independent of the configured Hash size.
constexpr std::size_t kHashfullSampleSlots = 1000;

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

HashMemorySnapshot default_memory_snapshot() noexcept {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0) {
        return HashMemorySnapshot{status.ullTotalPhys, status.ullAvailPhys, status.ullAvailPageFile};
    }
#endif
    return {};
}

std::size_t megabytes_to_bytes(std::size_t megabytes) noexcept {
    constexpr std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (megabytes > maximum / kBytesPerMegabyte) {
        return maximum;
    }
    return megabytes * kBytesPerMegabyte;
}

std::size_t bytes_to_megabytes(std::size_t bytes) noexcept {
    return bytes / kBytesPerMegabyte;
}

std::size_t u64_to_size(std::uint64_t value) noexcept {
    const std::uint64_t maximum = std::numeric_limits<std::size_t>::max();
    return static_cast<std::size_t>(std::min(value, maximum));
}

} // namespace

struct TranspositionTable::Storage {
    struct Segment {
        explicit Segment(std::size_t count, const HashAllocationFailureProvider& failure) {
            const std::size_t bytes = count * sizeof(TranspositionEntry);
            if (failure && failure(bytes)) {
                throw std::bad_alloc();
            }
            entries.resize(count);
        }

        std::vector<TranspositionEntry> entries;
    };

    explicit Storage(std::size_t megabytes, const HashAllocationFailureProvider& failure)
        : entries_per_segment(std::max<std::size_t>(
              kClusterSize,
              (kSegmentBytes / sizeof(TranspositionEntry)) / kClusterSize * kClusterSize)) {
        const std::size_t requested_bytes = megabytes_to_bytes(megabytes);
        const std::size_t requested_entries = std::max<std::size_t>(
            kClusterSize, requested_bytes / sizeof(TranspositionEntry));
        const std::size_t total_entries = std::max<std::size_t>(
            kClusterSize, requested_entries / kClusterSize * kClusterSize);
        const std::size_t segment_count =
            (total_entries + entries_per_segment - 1) / entries_per_segment;
        segments.reserve(segment_count);
        std::size_t remaining = total_entries;
        for (std::size_t index = 0; index < segment_count; ++index) {
            const std::size_t count = std::min(remaining, entries_per_segment);
            segments.push_back(std::make_unique<Segment>(count, failure));
            remaining -= count;
        }
        total_slot_count = total_entries;
        cluster_count = total_entries / kClusterSize;
        allocated_bytes = total_entries * sizeof(TranspositionEntry);
        size_mb = std::max<std::size_t>(1, bytes_to_megabytes(allocated_bytes));
    }

    [[nodiscard]] TranspositionEntry& at(std::size_t slot) noexcept {
        const std::size_t segment = slot / entries_per_segment;
        const std::size_t offset = slot % entries_per_segment;
        return segments[segment]->entries[offset];
    }

    [[nodiscard]] const TranspositionEntry& at(std::size_t slot) const noexcept {
        const std::size_t segment = slot / entries_per_segment;
        const std::size_t offset = slot % entries_per_segment;
        return segments[segment]->entries[offset];
    }

    std::vector<std::unique_ptr<Segment>> segments;
    std::array<std::shared_mutex, kStripeCount> stripes;
    const std::size_t entries_per_segment;
    std::size_t allocated_bytes = 0;
    std::size_t total_slot_count = 0;
    std::size_t cluster_count = 0;
    std::size_t size_mb = 0;
    std::uint16_t generation = 1;
    // `generation_age` is also the storage epoch for logical Clear Hash
    // invalidation.  A clear normally advances this fence without touching
    // the segments, which keeps repeated cold benchmark runs bounded by the
    // search work instead of the table size.
    std::uint16_t clear_epoch = 0;
};

TranspositionTable::TranspositionTable(std::size_t megabytes, HashMemoryPolicy policy)
    : memory_policy_(std::move(policy)) {
    std::lock_guard lock(maintenance_mutex_);
    (void)resize_locked(megabytes);
}

std::shared_ptr<TranspositionTable::Storage> TranspositionTable::snapshot() const noexcept {
    return std::atomic_load_explicit(&storage_, std::memory_order_acquire);
}

std::size_t TranspositionTable::normalized_size_mb(std::size_t megabytes) noexcept {
    return std::clamp(megabytes, kMinimumMegabytes, kMaximumMegabytes);
}

HashResizeResult TranspositionTable::set_size_mb(std::size_t megabytes) noexcept {
    std::lock_guard lock(maintenance_mutex_);
    return resize_locked(megabytes);
}

HashResizeResult TranspositionTable::resize_locked(std::size_t megabytes) noexcept {
    HashResizeResult result;
    result.requested_mb = megabytes;
    const std::size_t normalized = normalized_size_mb(megabytes);
    const bool request_was_clamped = normalized != megabytes;
    const auto current = snapshot();
    const std::size_t current_bytes = current == nullptr ? 0 : current->allocated_bytes;
    const std::size_t requested_bytes = megabytes_to_bytes(normalized);
    std::size_t allowed_bytes = requested_bytes;

    HashMemorySnapshot memory;
    try {
        memory = memory_policy_.memory_provider ? memory_policy_.memory_provider() :
            default_memory_snapshot();
    } catch (...) {
        memory = {};
    }
    result.total_physical_bytes = memory.total_physical_bytes;
    result.available_physical_bytes = memory.available_physical_bytes;
    result.available_commit_bytes = memory.available_commit_bytes;

    if (memory.total_physical_bytes != 0) {
        const std::size_t physical_cap = u64_to_size(memory.total_physical_bytes / 4ULL);
        if (physical_cap < allowed_bytes) {
            allowed_bytes = physical_cap;
            result.reason = HashResizeReason::physical_memory_cap;
        }
    }
    if (memory.available_commit_bytes != 0) {
        const std::size_t available_commit = u64_to_size(memory.available_commit_bytes);
        const std::size_t available_after_old = available_commit > current_bytes ?
            available_commit - current_bytes : 0;
        const std::size_t commit_cap = available_after_old / 2;
        if (commit_cap < allowed_bytes) {
            allowed_bytes = commit_cap;
            result.reason = HashResizeReason::commit_cap;
        }
    }

    if (allowed_bytes < kBytesPerMegabyte) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        if (result.reason == HashResizeReason::none) {
            result.reason = current == nullptr ? HashResizeReason::startup_unavailable :
                HashResizeReason::commit_cap;
        }
        return result;
    }

    const std::size_t candidate_mb = std::max<std::size_t>(1, bytes_to_megabytes(allowed_bytes));
    if (current != nullptr && current->size_mb == candidate_mb) {
        result.effective_mb = current->size_mb;
        result.allocated_bytes = current->allocated_bytes;
        result.segment_count = current->segments.size();
        result.status = candidate_mb < normalized || request_was_clamped ?
            HashResizeStatus::reduced : HashResizeStatus::unchanged;
        if (result.reason == HashResizeReason::none && request_was_clamped) {
            result.reason = HashResizeReason::request_clamped;
        }
        return result;
    }

    try {
        auto replacement = std::make_shared<Storage>(candidate_mb, memory_policy_.allocation_failure);
        result.effective_mb = replacement->size_mb;
        result.allocated_bytes = replacement->allocated_bytes;
        result.segment_count = replacement->segments.size();
        result.status = candidate_mb < normalized || request_was_clamped ?
            HashResizeStatus::reduced : HashResizeStatus::applied;
        if (result.reason == HashResizeReason::none && request_was_clamped) {
            result.reason = HashResizeReason::request_clamped;
        }
        std::atomic_store_explicit(&storage_, std::move(replacement), std::memory_order_release);
        return result;
    } catch (const std::bad_alloc&) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    } catch (const std::length_error&) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    } catch (...) {
        result.effective_mb = current == nullptr ? 0 : current->size_mb;
        result.allocated_bytes = current == nullptr ? 0 : current->allocated_bytes;
        result.segment_count = current == nullptr ? 0 : current->segments.size();
        result.status = current == nullptr ? HashResizeStatus::disabled : HashResizeStatus::unchanged;
        result.reason = HashResizeReason::allocation_failed;
        return result;
    }
}

std::size_t TranspositionTable::size_mb() const noexcept {
    const auto storage = snapshot();
    return storage == nullptr ? 0 : storage->size_mb;
}

std::size_t TranspositionTable::hashfull_permill() const noexcept {
    const auto storage = snapshot();
    if (storage == nullptr || storage->total_slot_count == 0) {
        return 0;
    }

    // Latch every stripe shared so the sample is consistent without blocking
    // probes or stores for longer than the scan itself, which is bounded by
    // kHashfullSampleSlots and spread with a stride so the whole table is
    // covered regardless of size.
    std::array<std::shared_lock<std::shared_mutex>, kStripeCount> stripe_locks;
    for (std::size_t index = 0; index < kStripeCount; ++index) {
        stripe_locks[index] = std::shared_lock<std::shared_mutex>(storage->stripes[index]);
    }

    const std::size_t sample_slots = std::min(kHashfullSampleSlots, storage->total_slot_count);
    const std::size_t stride = std::max<std::size_t>(1, storage->total_slot_count / sample_slots);
    std::size_t sampled = 0;
    std::size_t used = 0;
    for (std::size_t slot = 0; slot < storage->total_slot_count && sampled < sample_slots;
         slot += stride, ++sampled) {
        const TranspositionEntry& entry = storage->at(slot);
        if (entry.occupied && entry.generation_age == storage->clear_epoch) {
            ++used;
        }
    }
    return sampled == 0 ? 0 : used * 1000 / sampled;
}

void TranspositionTable::clear() noexcept {
    std::lock_guard maintenance_lock(maintenance_mutex_);
    const auto storage = snapshot();
    if (storage == nullptr || storage->total_slot_count == 0) {
        return;
    }

    std::array<std::unique_lock<std::shared_mutex>, kStripeCount> stripe_locks;
    for (std::size_t index = 0; index < kStripeCount; ++index) {
        stripe_locks[index] = std::unique_lock<std::shared_mutex>(storage->stripes[index]);
    }
    if (storage->clear_epoch == std::numeric_limits<std::uint16_t>::max()) {
        storage->clear_epoch = 0;
        for (const auto& segment : storage->segments) {
            std::fill(segment->entries.begin(), segment->entries.end(), TranspositionEntry{});
        }
    } else {
        ++storage->clear_epoch;
    }
}

void TranspositionTable::new_generation() noexcept {
    // Advance the per-search replacement epoch.  This counter is internal to
    // the table; it is neither the UCI protocol generation nor a search
    // request identity (see transposition_table.hpp).
    std::lock_guard maintenance_lock(maintenance_mutex_);
    const auto storage = snapshot();
    if (storage == nullptr || storage->total_slot_count == 0) {
        return;
    }

    std::array<std::unique_lock<std::shared_mutex>, kStripeCount> stripe_locks;
    for (std::size_t index = 0; index < kStripeCount; ++index) {
        stripe_locks[index] = std::unique_lock<std::shared_mutex>(storage->stripes[index]);
    }
    ++storage->generation;
    if (storage->generation == 0) {
        storage->generation = 1;
        for (const auto& segment : storage->segments) {
            std::fill(segment->entries.begin(), segment->entries.end(), TranspositionEntry{});
        }
    }
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, TranspositionBound bound,
                               Move best_move, int ply, bool pv) noexcept {
    const auto storage = snapshot();
    if (storage == nullptr || storage->cluster_count == 0) {
        return;
    }

    const std::size_t cluster = key % storage->cluster_count;
    const std::size_t first_slot = cluster * kClusterSize;
    std::unique_lock stripe_lock(storage->stripes[cluster % kStripeCount]);

    std::size_t empty_slot = storage->total_slot_count;
    std::size_t replacement_slot = first_slot;
    std::uint32_t replacement_priority = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t offset = 0; offset < kClusterSize; ++offset) {
        const std::size_t slot = first_slot + offset;
        TranspositionEntry& candidate = storage->at(slot);
        const bool valid = candidate.occupied &&
            candidate.generation_age == storage->clear_epoch;
        if (valid && candidate.key == key) {
            const bool keeps_deeper_entry = candidate.depth > depth;
            const bool keeps_equal_exact_entry = candidate.depth == depth &&
                candidate.bound == TranspositionBound::exact &&
                bound != TranspositionBound::exact;
            if (keeps_deeper_entry || keeps_equal_exact_entry) {
                candidate.generation = storage->generation;
                candidate.generation_age = storage->clear_epoch;
                return;
            }
            candidate = TranspositionEntry{key, depth, score_for_storage(score, ply), bound,
                                          best_move, storage->generation, true,
                                          storage->clear_epoch, pv};
            return;
        }
        if (!valid && empty_slot == storage->total_slot_count) {
            empty_slot = slot;
            continue;
        }

        // Prefer stale generations, then shallow entries.  The final key
        // component makes replacement deterministic when all other signals
        // tie, which is useful for fixed-depth reproducibility.
        const std::uint16_t age = static_cast<std::uint16_t>(
            storage->generation - candidate.generation);
        const std::uint32_t age_rank = age == 0 ? 1U : 0U;
        const std::uint32_t depth_rank = static_cast<std::uint32_t>(
            std::clamp(candidate.depth, 0,
                       static_cast<int>(std::numeric_limits<std::uint16_t>::max())));
        const std::uint32_t key_rank = static_cast<std::uint32_t>(candidate.key);
        const std::uint32_t priority = (age_rank << 24U) |
            (std::min<std::uint32_t>(depth_rank, 0xFFFFU) << 8U) |
            (key_rank & 0xFFU);
        if (priority < replacement_priority) {
            replacement_priority = priority;
            replacement_slot = slot;
        }
    }

    const std::size_t slot = empty_slot != storage->total_slot_count ?
        empty_slot : replacement_slot;
    TranspositionEntry& destination = storage->at(slot);
    if (empty_slot == storage->total_slot_count) {
        const bool older_generation = destination.generation != storage->generation;
        const bool deeper = depth > destination.depth;
        const bool deterministic_tie_break = depth == destination.depth && key < destination.key;
        if (!older_generation && !deeper && !deterministic_tie_break) {
            return;
        }
    }
    destination = TranspositionEntry{key, depth, score_for_storage(score, ply), bound, best_move,
                                     storage->generation, true, storage->clear_epoch, pv};
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key, int ply) const noexcept {
    const auto storage = snapshot();
    if (storage == nullptr || storage->cluster_count == 0) {
        return std::nullopt;
    }

    const std::size_t cluster = key % storage->cluster_count;
    const std::size_t first_slot = cluster * kClusterSize;
    std::shared_lock stripe_lock(storage->stripes[cluster % kStripeCount]);
    for (std::size_t offset = 0; offset < kClusterSize; ++offset) {
        const TranspositionEntry& entry = storage->at(first_slot + offset);
        if (!entry.occupied || entry.generation_age != storage->clear_epoch ||
            entry.key != key) {
            continue;
        }
        TranspositionEntry result = entry;
        result.score = score_for_probe(result.score, ply);
        result.generation_age = static_cast<std::uint16_t>(storage->generation - entry.generation);
        return result;
    }
    return std::nullopt;
}

} // namespace koi
