#pragma once

#include <array>
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

struct TranspositionEntry {
    std::uint64_t key = 0;
    int depth = 0;
    int score = 0;
    TranspositionBound bound = TranspositionBound::exact;
    Move best_move = Move::no_move();
    // Replacement epoch and probe-time age.  These have nothing to do with
    // UciController's protocol generation or SearchRequestIdentity: the epoch
    // is advanced once per search by TranspositionTable::new_generation() so
    // replacement prefers entries from previous searches.
    std::uint16_t generation = 0;
    bool occupied = false;
    // Distance from the current epoch, also reset by logical Clear Hash.  Kept
    // separate from `generation` so clear() can invalidate entries without
    // sweeping storage.
    std::uint16_t generation_age = 0;
    // PV provenance survives transposition.  Search uses it to keep the
    // stronger PV-side reduction/singular gates when a position is revisited
    // through a non-PV window; bound type alone cannot recover that context.
    bool pv = false;
};

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
    void clear() noexcept;
    // Starts the replacement epoch for a new search; search code calls this
    // once per search through detail::SearchTableAccess.  Unrelated to the UCI
    // protocol generation and SearchRequestIdentity (see TranspositionEntry).
    void new_generation() noexcept;
    void store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move,
               int ply = 0, bool pv = false) noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key, int ply = 0) const noexcept;

private:
    struct Storage;

    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] HashResizeResult resize_locked(std::size_t megabytes) noexcept;
    [[nodiscard]] std::shared_ptr<Storage> snapshot() const noexcept;

    static constexpr std::size_t kStripeCount = 64;

    mutable std::mutex maintenance_mutex_;
    std::shared_ptr<Storage> storage_;
    HashMemoryPolicy memory_policy_;
};

} // namespace koi
