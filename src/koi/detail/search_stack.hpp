#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "koi/move.hpp"

namespace koi::detail {

// The recursive alpha-beta path can reach the configured search depth and the
// bounded checked-position safety horizon. Keep this storage fixed and
// contiguous so a worker never allocates while descending the tree.
inline constexpr std::size_t kSearchStackCapacity = 128;
inline constexpr std::size_t kPrincipalVariationCapacity = 64;

struct SearchFrame {
    Move current_move = Move::no_move();
    Move previous_move = Move::no_move();
    int static_eval = 0;
    int move_count = 0;
    int reduction = 0;
    int extension = 0;
    bool in_check = false;
};

struct PrincipalVariation {
    std::array<Move, kPrincipalVariationCapacity> moves{};
    std::uint8_t length = 0;

    void clear() noexcept {
        length = 0;
    }

    void prepend(const Move move, const PrincipalVariation& child) noexcept {
        const std::size_t child_length = std::min<std::size_t>(
            child.length, kPrincipalVariationCapacity - 1);
        moves[0] = move;
        std::copy_n(child.moves.data(), child_length, moves.data() + 1);
        length = static_cast<std::uint8_t>(child_length + 1);
    }

    [[nodiscard]] std::vector<Move> to_vector() const {
        return {moves.begin(), moves.begin() + length};
    }
};

class SearchStack {
public:
    static constexpr std::size_t kCapacity = kSearchStackCapacity;

    [[nodiscard]] constexpr std::size_t capacity() const noexcept {
        return kCapacity;
    }

    [[nodiscard]] SearchFrame& frame(const std::size_t ply) noexcept {
        // Search bounds make this defensive clamp unreachable in a normal
        // search. It keeps diagnostic callers from producing undefined
        // behavior if they inspect a pathological ply value.
        return frames_[std::min(ply, kCapacity - 1)];
    }

    [[nodiscard]] const SearchFrame& frame(const std::size_t ply) const noexcept {
        return frames_[std::min(ply, kCapacity - 1)];
    }

    void reset() noexcept {
        frames_ = {};
    }

private:
    std::array<SearchFrame, kCapacity> frames_{};
};

} // namespace koi::detail
