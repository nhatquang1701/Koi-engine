#pragma once

#include <cstdint>
#include <optional>

#include "koi/game_state.hpp"
#include "koi/transposition_table.hpp"

namespace koi::detail {

// The public Position key deliberately models repetition identity and omits
// the halfmove clock.  Search bounds are different: a reversible continuation
// can reach a 50/75-move draw from one clock value but not another.  Reversible
// ancestor context matters as well: the same descendant can be a repetition
// in one history and a fresh position in another. Keep both pieces of rule
// state in the private search key without changing the public TT format or
// the identity key used by UCI/search-session cancellation.
[[nodiscard]] inline std::uint64_t search_transposition_key(
    const GameState& state) noexcept {
    std::uint64_t key = state.position_key() ^
        (static_cast<std::uint64_t>(state.halfmove_clock()) +
         0x9E3779B97F4A7C15ULL);
    key ^= (state.repetition_history_fingerprint() +
            0xD6E8FEB86659FD93ULL) * 0xA24BAED4963EE407ULL;
    if (state.repetition_history_suppressed()) {
        // A null branch deliberately suppresses legal repetition and clock
        // draws. Keep its artificial history domain separate even when the
        // board, halfmove clock, and real-history fingerprint collide with a
        // legal position.
        key ^= 0x6A09E667F3BCC909ULL;
    }
    key ^= key >> 30U;
    key *= 0xBF58476D1CE4E5B9ULL;
    key ^= key >> 27U;
    key *= 0x94D049BB133111EBULL;
    return key ^ (key >> 31U);
}

class SearchTableAccess final {
public:
    explicit SearchTableAccess(TranspositionTable& table, const bool enabled = true) noexcept
        : table_(&table), enabled_(enabled) {}

    void set_enabled(const bool enabled) noexcept {
        enabled_ = enabled;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_;
    }

    [[nodiscard]] std::optional<TranspositionEntry> probe(
        const std::uint64_t key, const int ply = 0) const noexcept {
        if (!enabled_) {
            return std::nullopt;
        }
        return table_->probe(key, ply);
    }

    void store(const std::uint64_t key, const int depth, const int score,
               const TranspositionBound bound, const Move best_move,
               const int ply = 0, const bool pv = false) noexcept {
        if (enabled_) {
            table_->store(key, depth, score, bound, best_move, ply, pv);
        }
    }

private:
    TranspositionTable* table_;
    bool enabled_;
};

} // namespace koi::detail
