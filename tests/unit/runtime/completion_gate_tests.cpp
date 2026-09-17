#include "koi/completion_gate.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "koi_test_support.hpp"

namespace {

using koi::CompletionCandidate;
using koi::CompletionDisposition;
using koi::CompletionGate;
using koi::CompletionOnce;
using koi::GameState;
using koi::Move;
using koi::SearchLimits;
using koi::SearchRequestIdentity;

using koi::test::require;

Move move(std::string_view uci) {
    return koi::test::require_value(Move::parse_uci(uci), "test move must parse");
}

SearchRequestIdentity identity(const GameState& root, std::uint64_t generation = 1) {
    return SearchRequestIdentity{generation, root.position_key(), root.fen()};
}

CompletionCandidate candidate(const GameState& root, std::string_view best,
                               std::uint64_t generation = 1) {
    CompletionCandidate value;
    value.best_move = move(best);
    value.pv = {*value.best_move};
    value.identity = identity(root, generation);
    return value;
}

void test_current_root_move_is_emitted() {
    const GameState root = GameState::startpos();
    CompletionGate gate;
    const auto result = gate.validate(root, SearchLimits{}, identity(root),
                                      candidate(root, "e2e4"));
    require(result.disposition == CompletionDisposition::emit,
            "a legal current-root result must be emitted");
    require(result.best_move == move("e2e4"), "the validated move must be preserved");
    require(!result.fallback_used, "a valid result must not use fallback");
}

void test_stale_generation_is_suppressed() {
    const GameState root = GameState::startpos();
    CompletionGate gate;
    const auto result = gate.validate(root, SearchLimits{}, identity(root, 2),
                                      candidate(root, "e2e4", 1));
    require(result.disposition == CompletionDisposition::suppress_stale,
            "a stale result must be suppressed");
    require(!result.best_move.has_value(), "stale output must not carry a move");
}

void test_mismatched_root_key_is_suppressed_even_for_a_legal_looking_move() {
    const auto parsed = GameState::from_fen(
        "r1bqkbnr/pppppppp/8/8/3P4/2n5/PPP2PPP/RNBQKBNR w KQkq - 0 2");
    require(parsed.has_value(), "the legal-looking d4c3 root fixture must parse");
    const GameState root = *parsed;
    CompletionCandidate result = candidate(root, "d4c3");
    const SearchRequestIdentity mismatched{result.identity.generation,
                                          result.identity.root_key ^ 1ULL,
                                          result.identity.root_fen};
    CompletionGate gate;
    const auto validation = gate.validate(root, SearchLimits{}, mismatched, result);
    require(validation.disposition == CompletionDisposition::suppress_stale,
            "a legal-looking move from a mismatched root key must be suppressed");
    require(!validation.best_move.has_value(),
            "root-key mismatch suppression must not carry a move to the controller");
}

void test_illegal_d4c3_uses_a_common_legal_fallback() {
    const auto parsed = GameState::from_fen(
        "rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1");
    require(parsed.has_value(), "the d4c3 fixture must parse");
    const GameState root = *parsed;
    CompletionCandidate invalid = candidate(root, "d4c3");
    CompletionGate gate;
    const auto result = gate.validate(root, SearchLimits{}, identity(root), invalid);
    require(result.disposition == CompletionDisposition::fallback,
            "an illegal d4c3 result must use the safe fallback path");
    require(result.fallback_used, "invalid candidates must be marked as fallback");
    require(result.best_move.has_value(), "a nonterminal root must have a fallback");
    require(*result.best_move != move("d4c3") && root.is_legal(*result.best_move),
            "the fallback must be legal and must not be d4c3");
}

void test_searchmoves_is_a_hard_restriction() {
    const GameState root = GameState::startpos();
    SearchLimits limits;
    limits.search_moves_specified = true;
    limits.search_moves = {move("d2d4")};
    CompletionGate gate;
    const auto result = gate.validate(root, limits, identity(root),
                                      candidate(root, "e2e4"));
    require(result.disposition == CompletionDisposition::fallback,
            "a candidate outside searchmoves must be replaced");
    require(result.best_move == move("d2d4"),
            "fallback must remain inside searchmoves");
}

void test_invalid_pv_cannot_be_emitted_as_valid_completion() {
    const GameState root = GameState::startpos();
    CompletionCandidate invalid = candidate(root, "e2e4");
    invalid.pv = {move("e2e4"), move("e7e5"), move("e7e6")};
    CompletionGate gate;
    const auto result = gate.validate(root, SearchLimits{}, identity(root), invalid);
    require(result.disposition == CompletionDisposition::fallback,
            "an invalid PV must not be emitted as a valid completion");
    require(result.fallback_used, "invalid PV must be recorded as a hard fallback");
}

void test_terminal_root_emits_0000() {
    const auto parsed = GameState::from_fen(
        "7k/6Q1/5K2/8/8/8/8/8 b - - 0 1");
    require(parsed.has_value(), "the checkmate fixture must parse");
    const GameState root = *parsed;
    CompletionGate gate;
    CompletionCandidate missing;
    missing.identity = identity(root);
    const auto result = gate.validate(root, SearchLimits{}, identity(root), missing);
    require(result.disposition == CompletionDisposition::emit && result.terminal,
            "a terminal root must emit a terminal completion");
    require(!result.best_move.has_value(), "terminal completion must represent 0000");
}

void test_completion_once_accepts_only_one_claim() {
    CompletionOnce once;
    require(once.try_claim(), "the first completion claim must succeed");
    require(!once.try_claim(), "a duplicate completion claim must be rejected");
}

void test_request_identity_helper_builds_and_matches() {
    const GameState root = GameState::startpos();
    const SearchRequestIdentity built = SearchRequestIdentity::from(root, 7);
    require(built.generation == 7 && built.root_key == root.position_key() &&
                built.root_fen == root.fen(),
            "from() must capture generation, root key, and root fen");
    require(built.matches(SearchRequestIdentity::from(root, 7)),
            "identical request identities must match");
    require(!built.matches(SearchRequestIdentity::from(root, 8)),
            "a different generation must not match");
    SearchRequestIdentity same = built;
    same.root_key ^= 1ULL;
    require(!built.matches(same), "a different root key must not match");
    same = built;
    same.root_fen = "8/8/8/8/8/8/8/8 w - - 0 1";
    require(!built.matches(same), "a different root fen must not match");
}

} // namespace

int main(int argc, char** argv) {
    const std::vector<koi::test::TestCase> tests{
        {"current root move", test_current_root_move_is_emitted},
        {"stale generation", test_stale_generation_is_suppressed},
        {"mismatched root key", test_mismatched_root_key_is_suppressed_even_for_a_legal_looking_move},
        {"illegal d4c3 fallback", test_illegal_d4c3_uses_a_common_legal_fallback},
        {"searchmoves restriction", test_searchmoves_is_a_hard_restriction},
        {"invalid PV fallback", test_invalid_pv_cannot_be_emitted_as_valid_completion},
        {"terminal 0000", test_terminal_root_emits_0000},
        {"completion once", test_completion_once_accepts_only_one_claim},
        {"request identity helper", test_request_identity_helper_builds_and_matches},
    };
    return koi::test::run_tests(tests, argc, argv);
}
