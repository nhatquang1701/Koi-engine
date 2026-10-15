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
//
// In exclusive mode the view also pins one session-scoped hazard lease
// (TranspositionTable::SessionLease) and serves probe()/store() through the
// pinned storage pointer directly, so the steady-state path runs no atomic
// instructions; the pin is released by set_exclusive(false) or destruction.
class SearchTableAccess final {
public:
    explicit SearchTableAccess(TranspositionTable& table, const bool enabled = true) noexcept
        : table_(&table), enabled_(enabled) {}

    // The session pin and the debug-only exclusive claim are tied to this
    // view's lifetime and must never be duplicated.
    SearchTableAccess(const SearchTableAccess&) = delete;
    SearchTableAccess& operator=(const SearchTableAccess&) = delete;
    SearchTableAccess(SearchTableAccess&&) = delete;
    SearchTableAccess& operator=(SearchTableAccess&&) = delete;

    // Releasing the exclusive pin and the debug-only claim must also happen
    // when the caller never invokes set_exclusive(false); release builds carry
    // no claim, and the pin is empty unless exclusive mode was enabled.
    ~SearchTableAccess() noexcept {
        release_pin();
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
    // Enabling also takes the session pin eagerly -- the pointer a per-access
    // lease would resolve while no resize completes -- so probe()/store() can
    // run on it directly.  A thread that cannot obtain a hazard slot still
    // gets a valid pin through Lease's maintenance-locked reference-counted
    // snapshot, which keeps this mode correct (only slower; a concurrent
    // resize may then complete).  Debug builds claim the table's single
    // exclusive session here; release builds keep only the mode flag and pin.
    void set_exclusive(const bool value) noexcept {
        if (value == exclusive_) {
            return;
        }
        if (value) {
#ifndef NDEBUG
            table_->claim_exclusive_session();
            exclusive_session_claimed_ = true;
#endif
            // Eager pin: acquired once and held until release.  While it is
            // held a resize cannot complete (see SessionLease), so the pinned
            // pointer stays the live one for the whole session.
            pin_.emplace(*table_);
        } else {
            release_pin();
#ifndef NDEBUG
            release_exclusive_session();
#endif
        }
        exclusive_ = value;
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
        if (exclusive_) {
            // Steady-state exclusive path: the session pin resolved the
            // storage once, so this probe performs no atomic instruction and
            // takes no stripe lock (probe_exclusive mirrors the locked scan).
            return table_->probe_exclusive(pin_->storage(), key, ply);
        }
        return table_->probe(key, ply, TranspositionAccessMode::locked);
    }

    // Advisory prefetch of the cluster a later probe will read; a disabled
    // view stays a complete no-op on the shared table.  Resolving the address
    // dereferences the live storage, and a resize may run during an active
    // search (the soak suite's resize regression test drives that contract),
    // so each mode uses its own lifetime guard: exclusive mode reads the
    // session-pinned storage, and the locked path takes the same hazard lease
    // as the probe.
    void prefetch(const std::uint64_t key) const noexcept {
        if (!enabled_) {
            return;
        }
        if (exclusive_) {
            table_->prefetch_exclusive(pin_->storage(), key);
            return;
        }
        table_->prefetch(key);
    }

    void store(const std::uint64_t key, const int depth, const int score,
               const TranspositionBound bound, const Move best_move,
               const int ply = 0, const bool pv = false,
               const int eval = kNoEvaluation) noexcept {
        if (!enabled_) {
            return;
        }
        if (exclusive_) {
            table_->store_exclusive(pin_->storage(), key, depth, score, bound, best_move,
                                    ply, pv, eval);
            return;
        }
        table_->store(key, depth, score, bound, best_move, ply, pv, eval,
                      TranspositionAccessMode::locked);
    }

private:
#ifndef NDEBUG
    void release_exclusive_session() noexcept {
        if (exclusive_session_claimed_) {
            table_->release_exclusive_session();
            exclusive_session_claimed_ = false;
        }
    }
#endif

    // Drops the exclusive session pin; the pin's destructor clears the hazard
    // slot it holds (and its fallback snapshot, if any).
    void release_pin() noexcept {
        pin_.reset();
    }

    TranspositionTable* table_;
    bool enabled_;
    // Exclusive access mode is off by default; see set_exclusive().
    bool exclusive_ = false;
    // Session-scoped hazard pin for exclusive mode.  Engaged by
    // set_exclusive(true) before exclusive_ is set, so probe()/store() can
    // rely on it whenever exclusive_ is true; released by set_exclusive(false)
    // and by this view's destructor so a resize can proceed afterwards.
    std::optional<TranspositionTable::SessionLease> pin_;
#ifndef NDEBUG
    // Debug-only: this view currently holds the table's exclusive claim.
    bool exclusive_session_claimed_ = false;
#endif
};

} // namespace koi::detail
