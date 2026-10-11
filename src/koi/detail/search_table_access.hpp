#pragma once

#include <cstddef>
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

// One search component's view of the shared transposition table.  Every TT
// call from search code (probes, stores, and the per-search generation
// advance) is made through this seam so a disabled view is a complete no-op
// on the shared table and the physical storage stays private to
// TranspositionTable.  The transposition "generation" advanced here is the
// table's replacement epoch, not the UCI protocol generation or a
// SearchRequestIdentity (see transposition_table.hpp).
class SearchTableAccess final {
public:
    explicit SearchTableAccess(TranspositionTable& table, const bool enabled = true) noexcept
        : table_(&table), enabled_(enabled) {}

    // A debug-only exclusive-session claim must be returned even when the
    // caller never invokes set_exclusive(false); release builds carry no
    // claim and the destructor body is empty.
    ~SearchTableAccess() noexcept {
#ifndef NDEBUG
        release_exclusive_session();
#endif
    }

    void set_enabled(const bool enabled) noexcept {
        enabled_ = enabled;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return enabled_;
    }

    // Enables or disables exclusive access mode for this view.  Exclusive
    // mode skips only the stripe lock, so it is valid only under the table's
    // single-live-search contract: the runner thread is the only TT toucher
    // while the session is active (UCI stop+join, koi-bench sequential).
    // Debug builds claim the table's single exclusive session here; release
    // builds keep only the mode flag.
    void set_exclusive(const bool value) noexcept {
        if (value == exclusive_) {
            return;
        }
        exclusive_ = value;
#ifndef NDEBUG
        if (value) {
            table_->claim_exclusive_session();
            exclusive_session_claimed_ = true;
        } else {
            release_exclusive_session();
        }
#endif
    }

    // Physical table presence, independent of this view's probe/store gate.
    [[nodiscard]] std::size_t size_mb() const noexcept {
        return table_->size_mb();
    }

    // Advances the shared replacement epoch for a new search.  Called once per
    // search by the orchestrator, at the same point as before this seam was
    // introduced; a disabled view must not age the shared table.
    void new_generation() noexcept {
        if (enabled_) {
            table_->new_generation();
        }
    }

    [[nodiscard]] std::optional<TranspositionEntry> probe(
        const std::uint64_t key, const int ply = 0) const noexcept {
        if (!enabled_) {
            return std::nullopt;
        }
        return table_->probe(key, ply, access_mode());
    }

    // Advisory prefetch of the cluster a later probe will read; a disabled
    // view stays a complete no-op on the shared table.
    void prefetch(const std::uint64_t key) const noexcept {
        if (enabled_) {
            table_->prefetch(key);
        }
    }

    void store(const std::uint64_t key, const int depth, const int score,
               const TranspositionBound bound, const Move best_move,
               const int ply = 0, const bool pv = false,
               const int eval = kNoEvaluation) noexcept {
        if (enabled_) {
            table_->store(key, depth, score, bound, best_move, ply, pv, eval, access_mode());
        }
    }

private:
    [[nodiscard]] TranspositionAccessMode access_mode() const noexcept {
        return exclusive_ ? TranspositionAccessMode::exclusive :
            TranspositionAccessMode::locked;
    }

#ifndef NDEBUG
    void release_exclusive_session() noexcept {
        if (exclusive_session_claimed_) {
            table_->release_exclusive_session();
            exclusive_session_claimed_ = false;
        }
    }
#endif

    TranspositionTable* table_;
    bool enabled_;
    // Exclusive access mode is off by default; see set_exclusive().
    bool exclusive_ = false;
#ifndef NDEBUG
    // Debug-only: this view currently holds the table's exclusive claim.
    bool exclusive_session_claimed_ = false;
#endif
};

} // namespace koi::detail
