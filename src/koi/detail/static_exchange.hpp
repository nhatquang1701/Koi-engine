#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "koi/detail/attack_tables.hpp"
#include "koi/game_state.hpp"
#include "koi/piece_values.hpp"
#include "koi/position.hpp"

namespace koi::detail {

// Returns the conservative material gain of a legal capture sequence on the
// destination square, from the moving side's perspective. Non-captures return
// zero.
[[nodiscard]] int static_exchange_gain(const GameState&, Move) noexcept;
[[nodiscard]] int static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;

constexpr int exchange_piece_value(PieceType type) noexcept {
    return piece_material_value(type);
}

constexpr PieceType promotion_piece_type(Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::knight:
        return PieceType::knight;
    case Promotion::bishop:
        return PieceType::bishop;
    case Promotion::rook:
        return PieceType::rook;
    case Promotion::queen:
        return PieceType::queen;
    case Promotion::none:
        return PieceType::pawn;
    }
    return PieceType::pawn;
}

constexpr int promotion_exchange_gain(Promotion promotion) noexcept {
    return exchange_piece_value(promotion_piece_type(promotion)) - exchange_piece_value(PieceType::pawn);
}

constexpr bool exchange_square_valid(int square) noexcept {
    return square >= 0 && square < 64;
}

constexpr std::size_t exchange_color_index(Color color) noexcept {
    return color == Color::white ? 0U : 1U;
}

constexpr std::uint64_t exchange_square_bit(int square) noexcept {
    return std::uint64_t{1} << square;
}

// Lightweight board view for static exchange evaluation.  Exchange evaluation
// needs the mailbox layout, the maintained piece bitboards, combined
// occupancy, the king squares, and the side to move.  Building the full
// evaluation feature set (attack maps, pawn structure, phase, and so on) for
// every node that contains a capture was wasted work.
//
// `piece_bitboards` follows the native layout: one mask per color and piece
// type, indexed by static_cast<std::size_t>(PieceType), with slot 0
// (PieceType::none) unused.  The snapshot is built once from the native
// position and then maintained in lockstep with the mailbox through every
// trial mutation, so attacker enumeration and the recapture legality test are
// table lookups instead of 64-square rescans.  The emitted attacker set and
// the legality predicate are exactly the mailbox versions, so the integer
// result is bit-identical.
struct ExchangeContext {
    std::array<Piece, 64> board{};
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::uint64_t occupied = 0;
    std::array<int, 2> king_squares{-1, -1};
    Color side_to_move = Color::white;
};

// Mirrors one mailbox store on `square` onto the maintained masks: any
// previous occupant is cleared from its piece bitboard and from occupancy,
// then `piece` (which may be empty) is installed in both views.  Every board
// mutation in the exchange loop goes through here, so the mailbox and the
// bitboards can never diverge.
inline void exchange_store_piece(ExchangeContext& context, int square, Piece piece) noexcept {
    const std::size_t index = static_cast<std::size_t>(square);
    const std::uint64_t square_bit = exchange_square_bit(square);
    const Piece previous = context.board[index];
    if (!previous.empty()) {
        context.piece_bitboards[exchange_color_index(previous.color)]
                               [static_cast<std::size_t>(previous.type)] &= ~square_bit;
        context.occupied &= ~square_bit;
    }
    context.board[index] = piece;
    if (!piece.empty()) {
        context.piece_bitboards[exchange_color_index(piece.color)]
                               [static_cast<std::size_t>(piece.type)] |= square_bit;
        context.occupied |= square_bit;
    }
}

// Builds the exchange baseline directly from the native position: the mailbox
// through copy_board_to, the piece masks through piece_bitboard, and combined
// occupancy as the union of the twelve masks.  No FeatureState or evaluation
// feature snapshot is consulted.
inline ExchangeContext exchange_context_from(const Position& position) noexcept {
    ExchangeContext context;
    position.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = exchange_color_index(color);
        std::uint64_t color_pieces = 0;
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            const std::uint64_t pieces = position.piece_bitboard(type, color);
            context.piece_bitboards[color_index][static_cast<std::size_t>(type)] = pieces;
            color_pieces |= pieces;
        }
        context.occupied |= color_pieces;
        const std::uint64_t king =
            context.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
        if (king != 0) {
            context.king_squares[color_index] = static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = position.side_to_move();
    return context;
}

// Same baseline through the public GameState surface, for search code that
// only holds a GameState.  GameState forwards copy_board_to, piece_bitboard,
// and side_to_move to the native Position, so this produces exactly the
// snapshot the Position overload above produces.
inline ExchangeContext exchange_context_from(const GameState& state) noexcept {
    ExchangeContext context;
    state.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::size_t color_index = exchange_color_index(color);
        std::uint64_t color_pieces = 0;
        for (const PieceType type : {PieceType::pawn, PieceType::knight, PieceType::bishop,
                                     PieceType::rook, PieceType::queen, PieceType::king}) {
            const std::uint64_t pieces = state.piece_bitboard(type, color);
            context.piece_bitboards[color_index][static_cast<std::size_t>(type)] = pieces;
            color_pieces |= pieces;
        }
        context.occupied |= color_pieces;
        const std::uint64_t king =
            context.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
        if (king != 0) {
            context.king_squares[color_index] = static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = state.side_to_move();
    return context;
}

// True when `attacker` attacks `target` in the trial state.  Uses the same
// five table queries as Position::square_attacked over the maintained masks:
// the reverse pawn table, the knight and king masks, and the sliding attacks
// generated through the current occupancy.  The ray masks include the first
// blocker, which matches the mailbox scan's "check the first occupied square,
// then stop" behavior, so the predicate is the same.
inline bool exchange_square_attacked_by(const ExchangeContext& context, int target,
                                        Color attacker) noexcept {
    if (!exchange_square_valid(target)) {
        return false;
    }
    const std::size_t color_index = exchange_color_index(attacker);
    const auto& pieces = context.piece_bitboards[color_index];
    if ((pawn_attackers_of(target, attacker == Color::white) &
         pieces[static_cast<std::size_t>(PieceType::pawn)]) != 0) {
        return true;
    }
    if ((knight_attacks(target) &
         pieces[static_cast<std::size_t>(PieceType::knight)]) != 0) {
        return true;
    }
    if ((king_attacks(target) &
         pieces[static_cast<std::size_t>(PieceType::king)]) != 0) {
        return true;
    }
    const std::uint64_t occupied = context.occupied;
    const std::uint64_t bishops =
        pieces[static_cast<std::size_t>(PieceType::bishop)] |
        pieces[static_cast<std::size_t>(PieceType::queen)];
    if ((bishop_attacks(target, occupied) & bishops) != 0) {
        return true;
    }
    const std::uint64_t rooks =
        pieces[static_cast<std::size_t>(PieceType::rook)] |
        pieces[static_cast<std::size_t>(PieceType::queen)];
    if ((rook_attacks(target, occupied) & rooks) != 0) {
        return true;
    }
    return false;
}

// Enumerates every piece of `attacker` that attacks `target` in the trial
// state by intersecting the target-centric attack tables with the side's
// maintained piece masks.  The old mailbox walk emitted the same set: pawns
// from the reverse table, knight and king masks, and the first blocker on each
// sliding ray (included by the ray construction).  Candidate order can differ
// from the mailbox walk only in which attacker is visited first; selection
// uses minimum exchange_piece_value with a lowest-square tie-break, so the
// chosen attacker is order-independent.
template <typename CandidateHandler>
void for_each_exchange_attacker(const ExchangeContext& context, int target,
                                Color attacker, CandidateHandler&& handler) noexcept {
    if (!exchange_square_valid(target)) {
        return;
    }
    const std::size_t color_index = exchange_color_index(attacker);
    const auto& pieces = context.piece_bitboards[color_index];
    const auto emit = [&handler](std::uint64_t candidates) noexcept {
        while (candidates != 0) {
            const int square = static_cast<int>(std::countr_zero(candidates));
            candidates &= candidates - 1;
            handler(square);
        }
    };

    emit(pawn_attackers_of(target, attacker == Color::white) &
         pieces[static_cast<std::size_t>(PieceType::pawn)]);
    emit(knight_attacks(target) & pieces[static_cast<std::size_t>(PieceType::knight)]);
    emit(king_attacks(target) & pieces[static_cast<std::size_t>(PieceType::king)]);
    const std::uint64_t occupied = context.occupied;
    emit(bishop_attacks(target, occupied) &
         (pieces[static_cast<std::size_t>(PieceType::bishop)] |
          pieces[static_cast<std::size_t>(PieceType::queen)]));
    emit(rook_attacks(target, occupied) &
         (pieces[static_cast<std::size_t>(PieceType::rook)] |
          pieces[static_cast<std::size_t>(PieceType::queen)]));
}

inline bool exchange_recapture_is_legal(ExchangeContext& context, Color side, int source,
                                        int target, Piece placed_piece) noexcept {
    const std::size_t side_index = exchange_color_index(side);
    const Piece moving_piece = context.board[static_cast<std::size_t>(source)];
    const Piece captured_piece = context.board[static_cast<std::size_t>(target)];
    const int saved_king_square = context.king_squares[side_index];

    exchange_store_piece(context, source, Piece{});
    exchange_store_piece(context, target, placed_piece);
    if (moving_piece.type == PieceType::king) {
        context.king_squares[side_index] = target;
    }
    const bool legal =
        !exchange_square_attacked_by(context, context.king_squares[side_index], opposite(side));
    exchange_store_piece(context, source, moving_piece);
    exchange_store_piece(context, target, captured_piece);
    context.king_squares[side_index] = saved_king_square;
    return legal;
}

inline int static_exchange_gain_from_features(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    ExchangeContext trial = context;
    const int source = move.from().index();
    const int target = move.to().index();
    if (!exchange_square_valid(source) || !exchange_square_valid(target)) {
        return 0;
    }

    const Piece moving_piece = trial.board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != trial.side_to_move ||
        moving_piece.type != initial.moving_piece || trial.king_squares[0] < 0 ||
        trial.king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_piece.color == Color::white ? -8 : 8;
    }
    if (!exchange_square_valid(captured_square)) {
        return 0;
    }

    constexpr std::size_t kMaximumExchangeDepth = 32;
    // Deliberately not zero-initialized: every read below touches only entries
    // 0..depth, and each entry is written before its first read (gains[0]
    // just below, gains[depth] right after ++depth), so clearing 32 slots per
    // capture was dead work.
    std::array<int, kMaximumExchangeDepth> gains;
    const Piece captured_piece = trial.board[static_cast<std::size_t>(captured_square)];
    gains[0] = exchange_piece_value(captured_piece.type) + promotion_exchange_gain(move.promotion());

    Piece placed_piece = moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_piece.type = promotion_piece_type(move.promotion());
    }
    exchange_store_piece(trial, source, Piece{});
    exchange_store_piece(trial, captured_square, Piece{});
    exchange_store_piece(trial, target, placed_piece);
    if (moving_piece.type == PieceType::king) {
        trial.king_squares[exchange_color_index(moving_piece.color)] = target;
    }

    Color side = opposite(moving_piece.color);
    std::size_t depth = 0;
    for (;;) {
        int attacker_square = -1;
        int attacker_value = std::numeric_limits<int>::max();
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;

        for_each_exchange_attacker(trial, target, side, [&](const int square) noexcept {
            const Piece candidate = trial.board[static_cast<std::size_t>(square)];
            Piece replacement = candidate;
            int candidate_promotion_gain = 0;
            if (candidate.type == PieceType::pawn && (target >> 3 == 0 || target >> 3 == 7)) {
                replacement.type = PieceType::queen;
                candidate_promotion_gain =
                    exchange_piece_value(PieceType::queen) - exchange_piece_value(PieceType::pawn);
            }
            if (!exchange_recapture_is_legal(trial, side, square, target, replacement)) {
                return;
            }

            const int value = exchange_piece_value(candidate.type);
            if (value < attacker_value ||
                (value == attacker_value && (attacker_square < 0 || square < attacker_square))) {
                attacker_square = square;
                attacker_value = value;
                recapturing_piece = replacement;
                recapture_promotion_gain = candidate_promotion_gain;
            }
        });

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        const Piece target_piece = trial.board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = exchange_piece_value(target_piece.type) - gains[depth - 1] +
            recapture_promotion_gain;

        const Piece departing_piece = trial.board[static_cast<std::size_t>(attacker_square)];
        exchange_store_piece(trial, attacker_square, Piece{});
        exchange_store_piece(trial, target, recapturing_piece);
        if (departing_piece.type == PieceType::king) {
            trial.king_squares[exchange_color_index(side)] = target;
        }
        side = opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }
    return gains[0];
}

// Evaluates one move against an already-built position baseline.  Callers that
// evaluate several moves of the same position (GameState::finalize_metadata,
// SearchMoveOrdering::order, SearchMovePicker) build the context once instead
// of rebuilding the mailbox and piece masks per capture; the score itself is
// identical to static_exchange_gain(GameState, MoveMetadata).
[[nodiscard]] inline int static_exchange_gain(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    return static_exchange_gain_from_features(context, initial);
}

} // namespace koi::detail
