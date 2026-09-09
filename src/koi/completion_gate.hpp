#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/search_types.hpp"

namespace koi {

enum class CompletionDisposition : std::uint8_t {
    emit,
    suppress_stale,
    fallback,
    quarantine,
};

enum class CompletionSource : std::uint8_t {
    search,
    book,
    tablebase,
    ponder,
};

struct CompletionCandidate {
    std::optional<Move> best_move;
    std::vector<Move> pv;
    std::optional<Move> ponder_move;
    SearchRequestIdentity identity;
    CompletionSource source = CompletionSource::search;
};

struct CompletionValidation {
    CompletionDisposition disposition = CompletionDisposition::quarantine;
    std::optional<Move> best_move;
    std::vector<Move> pv;
    std::optional<Move> ponder_move;
    bool identity_match = false;
    bool native_legal = false;
    bool shadow_legal = false;
    bool searchmoves_legal = false;
    bool pv_legal = false;
    bool ponder_legal = true;
    bool root_consistent = false;
    bool terminal = false;
    bool fallback_used = false;
    std::string reason;
};

class CompletionOnce {
public:
    [[nodiscard]] bool try_claim() noexcept;

private:
    std::atomic_bool claimed_ = false;
};

class CompletionGate {
public:
    [[nodiscard]] CompletionValidation validate(
        const GameState& root, const SearchLimits& limits,
        const SearchRequestIdentity& expected,
        const CompletionCandidate& candidate) const;

private:
    [[nodiscard]] static bool allowed_root_move(const SearchLimits& limits,
                                                const Move& move) noexcept;
    [[nodiscard]] static bool contains_uci(const std::vector<std::string>& moves,
                                           const Move& move) noexcept;
    [[nodiscard]] static std::optional<Move> common_fallback(
        const GameState& root, const SearchLimits& limits,
        const PositionConsistencySnapshot& snapshot);
    [[nodiscard]] static bool legal_pv(const GameState& root,
                                       const std::vector<Move>& pv) noexcept;
};

} // namespace koi
