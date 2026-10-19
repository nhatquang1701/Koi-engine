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
#include <thread>
#include <vector>

#include "koi/move.hpp"

namespace koi {

enum class TranspositionBound : std::uint8_t { exact, lower, upper };

// Access-mode selector for TranspositionTable::probe()/store().  `locked`
// takes the stripe lock and is required whenever another thread may touch the
// same table.  `exclusive` skips only the stripe lock: the caller guarantees
// one live search per table and that every access runs on that session's
// thread (UCI stop+join, koi-bench sequential).  The cluster scan and
// replacement logic stay identical in both modes, so a concurrent resize
// remains memory-safe.  detail::SearchTableAccess drives exclusive mode
// through a session-scoped hazard pin (SessionLease) and the
// probe_exclusive/store_exclusive entry points, which read the pinned storage
// pointer directly and so run no atomic instructions on the steady-state
// path.  Maintenance entry points (new_generation, hashfull, clear,
// set_size_mb) always stay locked; the prefetch hint routes by mode like
// probe (prefetch_exclusive rides the same pin).
enum class TranspositionAccessMode : std::uint8_t { locked, exclusive };

// Sentinel for "this entry carries no static evaluation".  Real evaluations are
// bounded far away from this value, so it is unambiguous.  The value is the
// std::int16_t minimum because the packed entry stores evaluations as int16.
inline constexpr std::int16_t kNoEvaluation = -32'768;

// Number of hazard-pointer slots one table can hand out to concurrent readers.
// Each store/probe publishes the storage it is about to use in one
// slot and revalidates it against the live pointer, and a resize waits for the
// retired storage to leave every slot before dropping it.  The pool is a
// thread-lifetime resource (acquired once per thread, released at thread exit),
// so the capacity only has to cover live threads, not accesses.
inline constexpr std::size_t kTranspositionHazardSlots = 1024;

struct TranspositionEntry {
    std::uint64_t key = 0;
    int score = 0;
    Move best_move = Move::no_move();
    // Unadjusted static evaluation from the side to move's perspective, or
    // kNoEvaluation when the entry was stored without one.  Search reuses it
    // to skip evaluate() on a transposition hit.  Field order and widths keep
    // the entry at 24 bytes, so a four-entry cluster spans 96 bytes.
    std::int16_t eval = kNoEvaluation;
    std::int8_t depth = 0;
    // Replacement epoch and probe-time age.  These have nothing to do with
    // UciController's protocol generation or SearchRequestIdentity: the epoch
    // is advanced once per search by TranspositionTable::new_generation() so
    // replacement prefers entries from previous searches.
    std::uint8_t generation = 0;
    // Distance from the current epoch, also reset by logical Clear Hash.  Kept
    // separate from `generation` so clear() can invalidate entries without
    // sweeping storage.
    std::uint8_t generation_age = 0;
    TranspositionBound bound = TranspositionBound::exact;
    bool occupied = false;
    // PV provenance survives transposition.  Search uses it to keep the
    // stronger PV-side reduction/singular gates when a position is revisited
    // through a non-PV window; bound type alone cannot recover that context.
    bool pv = false;
};

static_assert(sizeof(TranspositionEntry) == 24,
              "TranspositionEntry must stay cache-friendly");

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

// The search seam owns the exclusive access mode on behalf of one search; it
// also drives the debug-only exclusive-session claim below, which stays
// private to the table.
namespace detail {
class SearchTableAccess;
} // namespace detail

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
    // `mode` selects the stripe-lock policy, see TranspositionAccessMode.
    void store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move,
               int ply = 0, bool pv = false, int eval = kNoEvaluation,
               TranspositionAccessMode mode = TranspositionAccessMode::locked) noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(
        std::uint64_t key, int ply = 0,
        TranspositionAccessMode mode = TranspositionAccessMode::locked) const noexcept;
    // Software prefetch of the probe cluster.  Search calls this a few
    // instructions before the matching probe so the cache line fetch overlaps
    // the work in between; a disabled or empty table is a no-op.  The address
    // is resolved under the same hazard lease as probe(): the public service
    // seam supports resizing the table during a live search (the soak suite's
    // resize regression test drives that contract), so reading the retired
    // storage without a lease would be a use-after-free.  The "controller
    // joins the active search before a Hash resize" premise holds only for the
    // UCI path.
    void prefetch(std::uint64_t key) const noexcept;

private:
    struct Storage;

    // Access-mode implementations for the public probe()/store() entry
    // points: Locked == true takes the stripe lock, Locked == false skips
    // only that lock.  Both modes keep the Lease, the cluster scan,
    // replacement math, entry values, and generation/clear_epoch handling
    // character-identical, so one executing thread observes identical
    // entries; only lock instructions differ.  The exclusive steady state
    // driven by detail::SearchTableAccess does not use these templates: it
    // pins one SessionLease (see below) and calls probe_exclusive/
    // store_exclusive, whose bodies mirror the Locked == false instantiations
    // with the hazard lease replaced by that pinned pointer.
    template <bool Locked>
    [[nodiscard]] std::optional<TranspositionEntry> probe_impl(std::uint64_t key,
                                                               int ply) const noexcept;
    template <bool Locked>
    void store_impl(std::uint64_t key, int depth, int score, TranspositionBound bound,
                    Move best_move, int ply, bool pv, int eval) noexcept;

    // Exclusive-session entry points for detail::SearchTableAccess.  `storage`
    // is the pointer pinned once for the whole session by SessionLease (see
    // below); while the pin is held a resize cannot complete, so these read
    // the same objects a per-access lease would resolve and need neither the
    // hazard lease nor, in exclusive mode, the stripe lock.  The bodies mirror
    // probe_impl<false>/store_impl<false>; keep them character-identical
    // modulo the lease so both modes observe the same entries.
    [[nodiscard]] std::optional<TranspositionEntry> probe_exclusive(
        Storage* storage, std::uint64_t key, int ply) const noexcept;
    void store_exclusive(Storage* storage, std::uint64_t key, int depth, int score,
                         TranspositionBound bound, Move best_move, int ply, bool pv,
                         int eval) noexcept;
    void prefetch_exclusive(Storage* storage, std::uint64_t key) const noexcept;

    // Hazard-pointer lease for the store/probe hot paths.  A reader
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

    // Session-scoped hazard pin for exclusive access, held by
    // detail::SearchTableAccess.  Acquisition and release are exactly the
    // Lease constructor's publication/revalidation (one hazard-slot acquire)
    // and destructor's slot clear, but the pin is taken once per exclusive
    // session and held until set_exclusive(false) or the seam's destruction.
    // The pinned Storage* is the pointer any per-access lease would resolve
    // while no resize completes: resize_locked() publishes the replacement
    // first and only then waits for the retired storage to leave every hazard
    // slot, and this pin holds its slot, so generation, clear_epoch, stripes,
    // and entries cannot change owners while the pin is held.
    //
    // Memory safety and deadlock: under the documented contract (one live
    // search per table) a resize only runs between sessions, after the pin is
    // released.  A contract-violating resize cannot complete while pinned --
    // it waits on this slot and the retired storage stays alive, which is
    // strictly safer than the previous per-access lease (no use-after-free,
    // the violation turns into a wait).  No deadlock is possible on the
    // legitimate flow (UCI stop+join before Hash changes; koi-bench
    // sequential): the pinned session never waits on resize completion
    // itself, it only stops accessing the pin when the session ends, which
    // clears the slot and lets a waiting resize proceed.  A thread that
    // cannot obtain a hazard slot falls back to the maintenance-locked
    // reference-counted snapshot, exactly like Lease::adopt_snapshot; the pin
    // then keeps the storage alive by reference count and a concurrent resize
    // may complete, which is also safe.
    //
    // Invariant: while the pin is held, every TT access on the pinning thread
    // goes through probe_exclusive/store_exclusive; those take no lease, and a
    // per-access Lease on that thread would clear this slot early and give up
    // the resize wait.  Search satisfies it -- the exclusive seam serves all
    // of its accesses, and the runner's locked accesses happen before the
    // session or in the parallel branch where no pin exists.
    class SessionLease {
    public:
        explicit SessionLease(const TranspositionTable& table) noexcept : lease_(table) {
#ifndef NDEBUG
            // The pin must be acquired for the exclusive session claimed by
            // set_exclusive() and used on that owning thread; reuse the
            // exclusive-owner assertion so a misuse trips in debug builds.
            table.assert_exclusive_session_owner();
#endif
        }

        SessionLease(const SessionLease&) = delete;
        SessionLease& operator=(const SessionLease&) = delete;

        // Plain pointer to the pinned storage; reading it performs no atomic
        // operations.  It stays valid until this pin is released.
        [[nodiscard]] Storage* storage() const noexcept { return lease_.storage(); }

    private:
        // The hazard publication (constructor) and slot clear (destructor)
        // live in Lease; the session pin simply holds that lease open for as
        // long as the session lives.
        Lease lease_;
    };

    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] HashResizeResult resize_locked(std::size_t megabytes) noexcept;
    // Resolves this thread's hazard slot on first use and arranges its release
    // at thread exit.  A constant-initialized thread_local above keeps the hot
    // path free of dynamic-init guards.
    [[nodiscard]] static std::size_t register_thread_hazard_slot() noexcept;

#ifndef NDEBUG
    // Debug-only exclusive-session enforcement, driven by
    // detail::SearchTableAccess::set_exclusive.  It asserts (a) at most one
    // exclusive session exists per table at any time and (b) exclusive
    // probe()/store() calls run on the thread that claimed the session.
    // Release builds compile this out and rely on the documented contract
    // "one live search per table".
    void claim_exclusive_session() noexcept;
    void release_exclusive_session() noexcept;
    void assert_exclusive_session_owner() const noexcept;
    std::atomic<std::thread::id> exclusive_session_owner_{};
#endif

    friend class detail::SearchTableAccess;

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
