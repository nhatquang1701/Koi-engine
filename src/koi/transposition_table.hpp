#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "koi/move.hpp"

namespace koi {

enum class TranspositionBound : std::uint8_t { exact, lower, upper };

// Sentinel for "this entry carries no static evaluation".  Real evaluations are
// bounded far away from this value, so it is unambiguous.
inline constexpr int kNoEvaluation = -1'000'000'000;

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

    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] HashResizeResult resize_locked(std::size_t megabytes) noexcept;
    // Lock-free storage handle for the store/probe/prefetch hot paths.  The
    // owning shared_ptr stays under maintenance_mutex_, and replaced storages
    // are retired until a later resize so a reader that loaded the pointer
    // just before a resize still sees a live object.
    [[nodiscard]] Storage* hot_storage() const noexcept;

    static constexpr std::size_t kStripeCount = 64;
    // Replaced tables kept alive across resizes (normally at most one).
    static constexpr std::size_t kRetiredStorageLimit = 2;

    mutable std::mutex maintenance_mutex_;
    std::shared_ptr<Storage> storage_;
    std::atomic<Storage*> hot_storage_{nullptr};
    std::vector<std::shared_ptr<Storage>> retired_storages_;
    HashMemoryPolicy memory_policy_;
};

} // namespace koi
