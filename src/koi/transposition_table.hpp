#pragma once

#include <cstddef>
#include <cstdint>
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
    std::uint16_t generation = 0;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 16);

    void set_size_mb(std::size_t megabytes);
    [[nodiscard]] std::size_t size_mb() const noexcept;
    void clear() noexcept;
    void new_generation() noexcept;
    void store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move) noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key) const noexcept;

private:
    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;

    mutable std::mutex mutex_;
    std::vector<TranspositionEntry> entries_;
    std::size_t size_mb_ = 16;
    std::uint16_t generation_ = 1;
};

} // namespace koi
