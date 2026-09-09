#include "koi/completion_gate.hpp"

#include <algorithm>

namespace koi {

bool CompletionOnce::try_claim() noexcept {
    bool expected = false;
    return claimed_.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                             std::memory_order_relaxed);
}

bool CompletionGate::allowed_root_move(const SearchLimits& limits,
                                       const Move& move) noexcept {
    if (!limits.search_moves_specified) {
        return true;
    }
    return std::find(limits.search_moves.begin(), limits.search_moves.end(), move) !=
        limits.search_moves.end();
}

bool CompletionGate::contains_uci(const std::vector<std::string>& moves,
                                  const Move& move) noexcept {
    return std::find(moves.begin(), moves.end(), move.uci()) != moves.end();
}

bool CompletionGate::legal_pv(const GameState& root,
                              const std::vector<Move>& pv) noexcept {
    if (pv.empty()) {
        return false;
    }
    try {
        GameState position = root;
        for (const Move& move : pv) {
            if (!position.is_legal(move) || !position.make_move(move)) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Move> CompletionGate::common_fallback(
    const GameState& root, const SearchLimits& limits,
    const PositionConsistencySnapshot& snapshot) {
    std::vector<Move> legal = root.legal_moves();
    std::sort(legal.begin(), legal.end(), [](const Move& left, const Move& right) {
        return left.uci() < right.uci();
    });
    for (const Move& move : legal) {
        if (!allowed_root_move(limits, move) || !contains_uci(snapshot.shadow_legal_moves, move)) {
            continue;
        }
        GameState probe = root;
        if (probe.make_move(move)) {
            return move;
        }
    }
    return std::nullopt;
}

CompletionValidation CompletionGate::validate(
    const GameState& root, const SearchLimits& limits,
    const SearchRequestIdentity& expected,
    const CompletionCandidate& candidate) const {
    CompletionValidation result;
    result.identity_match = candidate.identity.generation == expected.generation &&
        candidate.identity.root_key == expected.root_key &&
        candidate.identity.root_fen == expected.root_fen &&
        expected.root_key == root.position_key() && expected.root_fen == root.fen();
    if (!result.identity_match) {
        result.disposition = CompletionDisposition::suppress_stale;
        result.reason = "identity_mismatch";
        return result;
    }

    try {
        const PositionConsistencySnapshot snapshot = root.consistency_snapshot();
        result.root_consistent = snapshot.consistent();
        result.terminal = snapshot.native_legal_moves.empty() &&
            snapshot.shadow_legal_moves.empty();
        if (candidate.best_move.has_value()) {
            result.native_legal = contains_uci(snapshot.native_legal_moves, *candidate.best_move);
            result.shadow_legal = contains_uci(snapshot.shadow_legal_moves, *candidate.best_move);
            result.searchmoves_legal = allowed_root_move(limits, *candidate.best_move);
        } else {
            result.searchmoves_legal = !limits.search_moves_specified ||
                limits.search_moves.empty();
        }
        result.pv_legal = candidate.best_move.has_value() ? legal_pv(root, candidate.pv) :
            result.terminal && candidate.pv.empty();

        if (candidate.best_move.has_value() && candidate.ponder_move.has_value()) {
            GameState after_best = root;
            result.ponder_legal = after_best.make_move(*candidate.best_move) &&
                after_best.is_legal(*candidate.ponder_move);
        }

        const bool candidate_valid = candidate.best_move.has_value() && result.native_legal &&
            result.shadow_legal && result.searchmoves_legal && result.pv_legal &&
            result.ponder_legal && result.root_consistent;
        if (candidate_valid) {
            result.disposition = CompletionDisposition::emit;
            result.best_move = candidate.best_move;
            result.pv = candidate.pv;
            result.ponder_move = candidate.ponder_move;
            result.reason = "validated";
            return result;
        }

        if (result.terminal) {
            result.disposition = CompletionDisposition::emit;
            result.reason = "terminal";
            return result;
        }

        result.best_move = common_fallback(root, limits, snapshot);
        result.fallback_used = result.best_move.has_value();
        if (result.best_move.has_value()) {
            result.pv = {*result.best_move};
            result.disposition = CompletionDisposition::fallback;
            result.reason = result.root_consistent ? "invalid_candidate" :
                "native_shadow_inconsistent";
            return result;
        }

        result.disposition = CompletionDisposition::quarantine;
        result.reason = "no_common_legal_move";
        return result;
    } catch (...) {
        result.disposition = CompletionDisposition::quarantine;
        result.reason = "validation_exception";
        return result;
    }
}

} // namespace koi
