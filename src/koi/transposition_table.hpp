#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
    std::uint16_t generation = 0;
    bool occupied = false;
};

class TranspositionTable {
public:
    explicit TranspositionTable(std::size_t megabytes = 512);

    void set_size_mb(std::size_t megabytes);
    [[nodiscard]] std::size_t size_mb() const noexcept;
    void clear() noexcept;
    void new_generation() noexcept;
    void store(std::uint64_t key, int depth, int score, TranspositionBound bound, Move best_move,
               int ply = 0) noexcept;
    [[nodiscard]] std::optional<TranspositionEntry> probe(std::uint64_t key, int ply = 0) const noexcept;

private:
    struct Storage;

    [[nodiscard]] static std::size_t normalized_size_mb(std::size_t megabytes) noexcept;
    [[nodiscard]] std::shared_ptr<Storage> snapshot() const noexcept;

    static constexpr std::size_t kStripeCount = 64;

    mutable std::mutex maintenance_mutex_;
    std::shared_ptr<Storage> storage_;
};

} // namespace koi
