#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/detail/compatibility_mirror.hpp"
#include "koi/detail/feature_state.hpp"
#include "koi/position.hpp"

#include "koi_test_support.hpp"

namespace {

using koi::GameState;
using koi::Move;
using koi::Square;

using koi::test::require;

Move require_move(std::string_view uci) {
    const auto move = Move::parse_uci(uci);
    require(move.has_value(), "test move must parse");
    return *move;
}

GameState require_state(std::string_view fen) {
    const auto state = GameState::from_fen(fen);
    require(state.has_value(), "test FEN must parse");
    return *state;
}

void test_fen_rule_state_and_position_facade() {
    const GameState castling = require_state("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 17 42");
    const GameState en_passant = require_state(
        "rnbqkbnr/pppp1ppp/8/3Pp3/8/8/PPP1PPPP/RNBQKBNR w KQkq e6 0 2");
    require(castling.castling_rights() == koi::kAllCastlingRights,
            "FEN castling rights must be preserved");
    require(castling.halfmove_clock() == 17 && castling.fullmove_number() == 42,
            "FEN clocks must be preserved");
    require(en_passant.en_passant_square() == *Square::parse("e6"),
            "FEN en-passant target must be preserved");

    const koi::Position position(castling.fen());
    require(position.castling_rights() == koi::kAllCastlingRights &&
                position.halfmove_clock() == 17 && position.fullmove_number() == 42,
            "Position must expose the same native rule state");
    require(position.piece_bitboard(koi::PieceType::king, koi::Color::white) == (std::uint64_t{1} << 4) &&
                position.piece_bitboard(koi::PieceType::king, koi::Color::black) == (std::uint64_t{1} << 60) &&
                position.piece_bitboard(koi::PieceType::rook, koi::Color::white) ==
                    ((std::uint64_t{1} << 0) | (std::uint64_t{1} << 7)) &&
                position.piece_bitboard(koi::PieceType::rook, koi::Color::black) ==
                    ((std::uint64_t{1} << 56) | (std::uint64_t{1} << 63)),
            "Position must expose incrementally maintained piece bitboards");
}

void test_compatibility_mirror_owns_only_shadow_state_and_history() {
    constexpr std::string_view fen =
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1";
    const auto parsed = GameState::from_fen(fen);
    require(parsed.has_value(), "ownership seam fixture must be valid");
    const auto move = Move::parse_uci("e1g1");
    require(move.has_value(), "ownership seam move must parse");
    const auto metadata = parsed->describe_move(*move);
    require(metadata.has_value(), "ownership seam move must have metadata");

    koi::Position native(fen);
    koi::detail::CompatibilityMirror mirror;
    require(mirror.set_fen(fen), "compatibility mirror must accept a valid native FEN");
    require(mirror.matches(native),
            "a freshly initialized compatibility mirror must match the native authority");
    require(mirror.apply_generated_move(*metadata, true),
            "the mirror must apply generated special-move metadata transactionally");

    koi::Position child(native);
    require(child.make_generated_move(metadata->move),
            "the native authority must apply the same generated special move");
    require(mirror.matches(child),
            "mirror state must match the independently advanced native state");
    require(mirror.history_size() == 1 && !mirror.last_move_is_null(),
            "normal mirror moves must add one non-null shadow history record");
    require(mirror.undo_move(), "the mirror must undo its own normal history record");
    require(mirror.matches(native) && mirror.history_size() == 0,
            "undoing the mirror must restore its source state without native mutation");

    require(mirror.apply_null_move(), "the mirror must own null-move shadow transitions");
    require(mirror.last_move_is_null(), "a null transition must be identifiable in shadow history");
    require(mirror.undo_null_move() && mirror.history_size() == 0,
            "the mirror null transition must be independently reversible");

    constexpr std::string_view start_fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    require(mirror.set_fen(start_fen),
            "the mirror failure fixture must reset to a legal start position");
    require(mirror.apply_move(require_move("e2e4")),
            "the mirror failure fixture must establish a valid shadow history record");
    const std::uint64_t failed_move_key = mirror.position_key();
    const std::size_t failed_move_history = mirror.history_size();
    require(!mirror.apply_move(require_move("e2e5")),
            "an illegal mirror move must be rejected without a transition");
    require(mirror.position_key() == failed_move_key &&
                mirror.history_size() == failed_move_history,
            "a rejected mirror move must preserve the prior shadow state and history");
}

koi::PositionFeatures ownership_feature_builder(const koi::Position& position) noexcept {
    koi::PositionFeatures features{};
    features.side_to_move = position.side_to_move();
    features.fullmove_number = position.fullmove_number();
    return features;
}

void test_feature_state_owns_cache_publication_and_invalidation() {
    const koi::Position native;
    koi::detail::FeatureState cache;
    const std::uint64_t key = native.position_key();

    const koi::PositionFeatures first = cache.get_or_compute(
        0, key, native, ownership_feature_builder);
    const koi::PositionFeatures second = cache.get_or_compute(
        0, key, native, ownership_feature_builder);
    require(first.side_to_move == koi::Color::white && second.fullmove_number == 1,
            "feature ownership seam must return the builder's published value");
    require(cache.cache_misses() == 1,
            "a repeated feature request must be served from the published cache without rebuilding");

    cache.invalidate(0);
    (void)cache.get_or_compute(0, key, native, ownership_feature_builder);
    require(cache.cache_misses() == 2,
            "invalidating one position slot must force exactly one subsequent rebuild");
}

void test_claimable_and_automatic_repetition_thresholds() {
    GameState state = GameState::startpos();
    constexpr std::string_view cycle[] = {"g1f3", "g8f6", "f3g1", "f6g8"};
    for (const std::string_view uci : cycle) require(state.make_move(require_move(uci)), "cycle move must be legal");
    require(state.repetition_count() == 2 && state.is_repetition_sensitive() &&
                !state.can_claim_threefold_repetition() && !state.is_terminal(),
            "twofold repetition must be sensitive but non-terminal");

    for (const std::string_view uci : cycle) require(state.make_move(require_move(uci)), "cycle move must be legal");
    require(state.repetition_count() == 3 && state.can_claim_threefold_repetition() &&
                state.draw_status() == koi::DrawStatus::claimable_threefold && !state.is_terminal(),
            "threefold repetition must be claimable, not automatic");

    for (int iteration = 0; iteration < 2; ++iteration) {
        for (const std::string_view uci : cycle) {
            require(state.make_move(require_move(uci)), "cycle move must be legal");
        }
    }
    require(state.repetition_count() == 5 && state.is_automatic_fivefold_repetition() &&
                state.draw_status() == koi::DrawStatus::automatic_fivefold && state.is_terminal(),
            "fivefold repetition must be automatic and terminal");
}

void test_move_clock_checkmate_and_dead_position_rules() {
    GameState fifty = require_state("4k3/8/8/8/8/8/8/R3K3 w - - 99 1");
    require(fifty.make_move(require_move("a1a2")) && fifty.can_claim_fifty_move_draw() &&
                fifty.draw_status() == koi::DrawStatus::claimable_fifty_move && !fifty.is_terminal(),
            "the 50-move threshold must remain claimable");

    const GameState seventy_five = require_state("4k3/8/8/8/8/8/8/R3K3 w - - 150 1");
    require(seventy_five.is_automatic_seventy_five_move_draw() && seventy_five.is_terminal(),
            "the 75-move threshold must be automatic and terminal");

    const GameState checkmate = require_state("7k/6Q1/5K2/8/8/8/8/8 b - - 150 1");
    require(!checkmate.is_automatic_seventy_five_move_draw() &&
                checkmate.draw_status() == koi::DrawStatus::none && checkmate.is_terminal(),
            "checkmate must take precedence over the 75-move rule");

    const GameState kings = require_state("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    require(kings.is_dead_position() && kings.draw_status() == koi::DrawStatus::dead_position &&
                kings.is_terminal(), "king versus king must be a dead position");

    const GameState locked = require_state(
        "8/2b1k3/7p/p1p1p2P/PpP1P3/1P1BK3/8/8 w - - 0 1");
    require(locked.is_dead_position() && locked.is_terminal(),
            "the known locked pawn wall must be recognized conservatively");
    const GameState live = require_state("4k3/8/8/8/8/8/4P3/4K1B1 w - - 0 1");
    require(!live.is_dead_position(), "a mobile pawn position must remain live");
}

void test_make_unmake_and_special_move_rules() {
    GameState state = require_state("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 17 42");
    const std::string original_fen = state.fen();
    const auto original_key = state.position_key();
    require(state.make_move(require_move("e1g1")) && state.unmake_move(),
            "castling must make and unmake");
    require(state.fen() == original_fen && state.position_key() == original_key &&
                state.castling_rights() == koi::kAllCastlingRights,
            "castling unmake must restore all rule state");

    GameState promotions = require_state("k7/1P6/8/8/8/8/8/7K w - - 0 1");
    for (const std::string_view uci : {"b7b8n", "b7b8b", "b7b8r", "b7b8q"}) {
        const Move move = require_move(uci);
        require(promotions.is_legal(move) && promotions.make_move(move) && promotions.unmake_move(),
                "all promotion types must make and unmake legally");
    }
    const GameState ep_pin = require_state("7k/8/8/r4pPK/8/8/8/8 w - f6 0 1");
    require(!ep_pin.is_legal(require_move("g5f6")),
            "en-passant exposing the king to a rook must be rejected");
}

void test_en_passant_identity_requires_a_legal_capture() {
    GameState pinned = require_state("7k/5p2/8/r5PK/8/8/8/8 b - - 0 1");
    const Move double_push = require_move("f7f5");
    require(pinned.is_legal(double_push),
            "the pinned en-passant fixture must recognize the pawn double push as legal");
    const bool moved = pinned.make_move(double_push);
    require(moved,
            "the pinned en-passant fixture must allow the pawn double push");
    const auto synchronized = pinned.consistency_snapshot();
    require(synchronized.consistent(),
            "canonical unusable en-passant state must remain native/shadow consistent");
    require(!pinned.is_legal(require_move("g5f6")),
            "the adjacent pawn must remain unable to capture en passant through its pin");

    const GameState without_target = require_state("7k/8/8/r4pPK/8/8/8/8 w - - 0 2");
    require(pinned.en_passant_square().index() == Square::kInvalid &&
                pinned.position_key() == without_target.position_key(),
            "an unusable en-passant target must not alter the native repetition identity");

    const GameState capturable = require_state("7k/8/8/5pP1/7K/8/8/8 w - f6 0 2");
    const GameState capturable_without_target = require_state(
        "7k/8/8/5pP1/7K/8/8/8 w - - 0 2");
    require(capturable.is_legal(require_move("g5f6")) &&
                capturable.position_key() != capturable_without_target.position_key(),
            "a legal en-passant capture must remain part of the native repetition identity");
}

void test_generated_move_transaction_preserves_shadow_consistency() {
    const GameState root = GameState::startpos();
    const std::string original_fen = root.fen();
    const std::uint64_t original_key = root.position_key();
    const std::vector<koi::MoveMetadata> moves = root.legal_moves_with_metadata();
    require(moves.size() == 20, "generated move transaction fixture must contain all start moves");

    for (const koi::MoveMetadata& metadata : moves) {
        GameState child = root;
        require(child.make_generated_move(metadata),
                "metadata from the native legal generator must apply transactionally");
        require(child.consistency_snapshot().consistent(),
                "generated move transaction must keep native and shadow state synchronized");
        require(child.unmake_move(), "generated move transaction must unmake");
        require(child.fen() == original_fen && child.position_key() == original_key,
                "generated move transaction must restore the exact root state");
    }
}

void test_search_move_transaction_preserves_shadow_consistency() {
    const GameState root = GameState::startpos();
    const std::string original_fen = root.fen();
    const std::uint64_t original_key = root.position_key();
    const std::vector<koi::MoveMetadata> moves = root.legal_moves_with_metadata();
    require(moves.size() == 20, "search move transaction fixture must contain all start moves");

    for (const koi::MoveMetadata& metadata : moves) {
        GameState child = root;
        require(child.make_search_move(metadata),
                "metadata from the native legal generator must apply through the search path");
        require(child.consistency_snapshot().consistent(),
                "search move transaction must keep native and shadow state synchronized");
        require(child.unmake_move(), "search move transaction must unmake");
        require(child.fen() == original_fen && child.position_key() == original_key,
                "search move transaction must restore the exact root state");
    }
}

void test_fixed_buffer_legal_generation_matches_vector_api() {
    const koi::Position position;
    std::array<Move, koi::kMaximumLegalMoves> buffer{};
    const std::size_t count = position.legal_moves_into(buffer);
    const std::vector<Move> vector_moves = position.legal_moves();
    require(count == vector_moves.size(),
            "fixed-buffer legal generation must return the vector move count");
    for (std::size_t index = 0; index < count; ++index) {
        require(buffer[index] == vector_moves[index],
                "fixed-buffer legal generation must preserve move ordering");
    }
}

struct TestCase { std::string_view name; void (*run)(); };

} // namespace

int main() {
    const std::vector<TestCase> tests{
        {"compatibility mirror ownership", test_compatibility_mirror_owns_only_shadow_state_and_history},
        {"feature state ownership", test_feature_state_owns_cache_publication_and_invalidation},
        {"FEN rule state and Position facade", test_fen_rule_state_and_position_facade},
        {"repetition thresholds", test_claimable_and_automatic_repetition_thresholds},
        {"clock, checkmate, and dead positions", test_move_clock_checkmate_and_dead_position_rules},
        {"make/unmake and special moves", test_make_unmake_and_special_move_rules},
        {"legal en-passant repetition identity", test_en_passant_identity_requires_a_legal_capture},
        {"generated move transaction", test_generated_move_transaction_preserves_shadow_consistency},
        {"search move transaction", test_search_move_transaction_preserves_shadow_consistency},
        {"fixed-buffer legal generation", test_fixed_buffer_legal_generation_matches_vector_api},
    };
    for (const TestCase& test : tests) {
        try {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
