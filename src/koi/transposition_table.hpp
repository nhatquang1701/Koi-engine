#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

#include "koi/move.hpp"

namespace koi {

enum class TranspositionBound : std::uint8_t { exact, lower, upper };

// Sentinel for "this entry carries no static evaluation".  Real evaluations are
// bounded far away from this value, so it is unambiguous.
inline constexpr int kNoEvaluation = -1'000'000'000;

// Number of hazard-pointer slots one table can hand out to concurrent readers.
// Each store/probe/prefetch publishes the storage it is about to use in one
// slot and revalidates it against the live pointer, and a resize waits for the
// retired storage to leave every slot before dropping it.  The pool is a
// thread-lifetime resource (acquired once per thread, released at thread exit),
// so the capacity only has to cover live threads, not accesses.
inline constexpr std::size_t kTranspositionHazardSlots = 1024;

struct TranspositionEntry {
    std::uint64_t key = 0;
    int depth = 0;
    int score = 0;
    // Unadjusted static evaluation from the side to move's perspective, or
    // kNoEvaluation when the entry was stored without one.  Search reuses it
    // to skip evaluate() on a transposition hit.  Field order below keeps the
    // entry at 32 bytes so a cluster still spans two cache lines.
    int eval = kNoEvaluation;
    Move best_move = Move::no_move();
    // Replacement epoch and probe-time age.  These have nothing to do with
    // UciController's protocol generation or SearchRequestIdentity: the epoch
    // is advanced once per search by TranspositionTable::new_generation() so
    // replacement prefers entries from previous searches.
    std::uint16_t generation = 0;
    // Distance from the current epoch, also reset by logical Clear Hash.  Kept
    // separate from `generation` so clear() can invalidate entries without
    // sweeping storage.
    std::uint16_t generation_age = 0;
    TranspositionBound bound = TranspositionBound::exact;
    bool occupied = false;
    // PV provenance survives transposition.  Search uses it to keep the
    // stronger PV-side reduction/singular gates when a position is revisited
    // through a non-PV window; bound type alone cannot recover that context.
    bool pv = false;
};

static_assert(sizeof(TranspositionEntry) == 32,
              "TranspositionEntry must stay cache-line friendly");

enum class HashResizeStatus : std::uint8_t {
    applied,
    reduced,
    unchanged,
    disabled
};

enum class HashResizeReason : std::uint8_t {
    none,
    request_clamped,
    physical_memory_cap,
    commit_cap,
    allocation_failed,
    startup_unavailable
};

struct HashResizeResult {
    std::size_t requested_mb = 0;
    std::size_t effective_mb = 0;
    std::size_t allocated_bytes = 0;
    std::size_t segment_count = 0;
    std::uint64_t total_physical_bytes = 0;
    std::uint64_t available_physical_bytes = 0;
    std::uint64_t available_commit_bytes = 0;
    HashResizeStatus status = HashResizeStatus::unchanged;
    HashResizeReason reason = HashResizeReason::none;
};

struct HashMemorySnapshot {
    std::uint64_t total_physical_bytes = 0;
    std::uint64_t available_physical_bytes = 0;
    std::uint64_t available_commit_bytes = 0;
};

using HashMemoryProvider = std::function<HashMemorySnapshot()>;
using HashAllocationFailureProvider = std::function<bool(std::size_t)>;

struct HashMemoryPolicy {
    HashMemoryProvider memory_provider;
    HashAllocationFailureProvider allocation_failure;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 512, HashMemoryPolicy policy = {});

    [[nodiscard]] HashResizeResult set_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] std::size_t size_mb() const noexcept;
    // Approximate occupancy in permill (0..1000) of entries from the current
    // clear epoch.  Samples a bounded number of slots (see
    // kHashfullSampleSlots) so the cost is independent of table size; used to
    // report UCI `info ... hashfull`.  Clear Hash logically invalidates all
    // entries, so the sampled value drops to zero after clear().
    [[nodiscard]] std::size_t hashfull_permill() const noexcept;
    void clear() noexcept;
    // Starts the replacement epoch for a new search; search code calls this
    // once per search through detail::SearchTableAccess.  Unrelated to the UCI
    // protocol generation and SearchRequestIdentity (see TranspositionEntry).
    void new_generation() noexcept;
    // `eval` is the unadjusted static evaluation for the position, or
    // kNoEvaluation when the caller has none (checked nodes, bound-only
    // stores).  It is stored so a later probe can skip evaluate().
    void store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move,
               int ply = 0, bool pv = false, int eval = kNoEvaluation) noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key, int ply = 0) const noexcept;
    // Software prefetch of the probe cluster.  Search calls this a few
    // instructions before the matching probe so the cache line fetch overlaps
    // the work in between; a disabled or empty table is a no-op.
    void prefetch(std::uint64_t key) const noexcept;

private:
    struct Storage;

    // Hazard-pointer lease for the store/probe/prefetch hot paths.  A reader
    // publishes the storage pointer it is about to use in one hazard slot and
    // then revalidates it against the live pointer; a resize swaps the live
    // pointer first and only then waits for the retired storage to leave every
    // slot, so a reader can never dereference freed storage.  A thread that
    // cannot obtain a slot (pool exhausted) falls back to a maintenance-locked
    // shared_ptr snapshot, so correctness never depends on hazard capacity.
    class Lease {
    public:
        explicit Lease(const TranspositionTable& table) noexcept : table_(&table) {
            std::size_t slot = cached_hazard_slot_;
            if (slot >= kTranspositionHazardSlots) {
                slot = register_thread_hazard_slot();
            }
            if (slot < kTranspositionHazardSlots) {
                hazard_slot_ = slot;
                publish(table);
                return;
            }
            adopt_snapshot(table);
        }

        ~Lease() {
            if (hazard_slot_ < kTranspositionHazardSlots) {
                table_->hazards_[hazard_slot_].storage.store(nullptr, std::memory_order_release);
            }
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        [[nodiscard]] Storage* storage() const noexcept { return storage_; }

    private:
        void publish(const TranspositionTable& table) noexcept {
            // Hazard fast path: when this thread's slot already pins the live
            // storage, no publication or revalidation is needed.  The pin was
            // taken by an earlier access and a resize cannot complete while
            // the slot still holds the retired pointer, so the value stays
            // valid for this whole lease.
            Storage* const live = table.hot_storage_.load(std::memory_order_acquire);
            if (live != nullptr &&
                table.hazards_[hazard_slot_].storage.load(std::memory_order_relaxed) == live) {
                storage_ = live;
                return;
            }
            for (;;) {
                Storage* const candidate = table.hot_storage_.load(std::memory_order_acquire);
                if (candidate == nullptr) {
                    table.hazards_[hazard_slot_].storage.store(nullptr, std::memory_order_release);
                    return;
                }
                table.hazards_[hazard_slot_].storage.store(candidate, std::memory_order_release);
                // Revalidation closes the window where a resize swapped the
                // live pointer between the load and the hazard publication:
                // the writer scanned the slots before this one was published,
                // so only a reader that re-observes its candidate may
                // dereference it.
                if (table.hot_storage_.load(std::memory_order_acquire) == candidate) {
                    storage_ = candidate;
                    return;
                }
            }
        }

        // Cold path for a thread that cannot obtain a hazard slot; keeps the
        // storage alive with a maintenance-locked reference-counted snapshot.
        void adopt_snapshot(const TranspositionTable& table) noexcept;

        const TranspositionTable* table_ = nullptr;
        Storage* storage_ = nullptr;
        std::size_t hazard_slot_ = kTranspositionHazardSlots;
        std::shared_ptr<Storage> fallback_;
    };

    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] HashResizeResult resize_locked(std::size_t megabytes) noexcept;
    // Resolves this thread's hazard slot on first use and arranges its release
    // at thread exit.  A constant-initialized thread_local above keeps the hot
    // path free of dynamic-init guards.
    [[nodiscard]] static std::size_t register_thread_hazard_slot() noexcept;

    static constexpr std::size_t kStripeCount = 64;

    mutable std::mutex maintenance_mutex_;
    std::shared_ptr<Storage> storage_;
    std::atomic<Storage*> hot_storage_{nullptr};
    // One hazard slot per concurrent reader, see Lease.  Slots are assigned by
    // a process-wide thread-lifetime pool, so a thread always publishes in the
    // same slot and a resize can wait on a bounded array.
    //
    // Slots are cache-line padded: unpadded 8-byte atomics let two readers in
    // adjacent slots ping-pong the same line on every table access, which
    // measured as a ~9% multi-thread regression.
    struct alignas(64) HazardSlot {
        std::atomic<Storage*> storage{nullptr};
    };

    mutable std::array<HazardSlot, kTranspositionHazardSlots> hazards_{};
    // This thread's hazard slot index, resolved once per thread.  Constant
    // initialized so reading it in Lease does not run a TLS guard check; the
    // sentinel means "not resolved yet".
    inline static thread_local std::size_t cached_hazard_slot_ = kTranspositionHazardSlots;
    HashMemoryPolicy memory_policy_;
};

} // namespace koi
