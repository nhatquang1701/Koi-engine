#include "koi/transposition_table.hpp"

#include <algorithm>
#include <limits>

namespace koi {

TranspositionTable::TranspositionTable(std::size_t megabytes) {
    set_size_mb(megabytes);
}

std::size_t TranspositionTable::normalized_size_mb(std::size_t megabytes) noexcept {
    return std::clamp(megabytes, std::size_t{1}, std::size_t{4096});
}

void TranspositionTable::set_size_mb(std::size_t megabytes) {
    const std::size_t normalized = normalized_size_mb(megabytes);
    const std::size_t entry_count = std::max<std::size_t>(1, (normalized * 1024ULL * 1024ULL) / sizeof(TranspositionEntry));
    std::vector<TranspositionEntry> entries(entry_count);

    std::lock_guard lock(mutex_);
    entries_ = std::move(entries);
    size_mb_ = normalized;
    generation_ = 1;
}

std::size_t TranspositionTable::size_mb() const noexcept {
    std::lock_guard lock(mutex_);
    return size_mb_;
}

void TranspositionTable::clear() noexcept {
    std::lock_guard lock(mutex_);
    std::fill(entries_.begin(), entries_.end(), TranspositionEntry{});
}

void TranspositionTable::new_generation() noexcept {
    std::lock_guard lock(mutex_);
    ++generation_;
    if (generation_ == 0) {
        generation_ = 1;
        std::fill(entries_.begin(), entries_.end(), TranspositionEntry{});
    }
}

void TranspositionTable::store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move) noexcept {
    std::lock_guard lock(mutex_);
    if (entries_.empty()) {
        return;
    }

    TranspositionEntry& existing = entries_[key % entries_.size()];
    const bool empty = existing.key == 0;
    const bool same_key = existing.key == key;
    const bool older_generation = existing.generation != generation_;
    const bool deeper = depth > existing.depth;
    const bool deterministic_tie_break = depth == existing.depth && key < existing.key;
    if (!empty && !same_key && !older_generation && !deeper && !deterministic_tie_break) {
        return;
    }

    existing = TranspositionEntry{key, depth, score, bound, best_move, generation_};
}

std::optional<TranspositionEntry> TranspositionTable::probe(std::uint64_t key) const noexcept {
    std::lock_guard lock(mutex_);
    if (entries_.empty()) {
        return std::nullopt;
    }

    const TranspositionEntry& entry = entries_[key % entries_.size()];
    if (entry.key != key) {
        return std::nullopt;
    }
    return entry;
}

} // namespace koi
