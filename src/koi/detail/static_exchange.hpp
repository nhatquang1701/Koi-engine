#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

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

inline bool exchange_square_attacked_by(const std::array<Piece, 64>& board, int target, Color attacker) noexcept {
    if (!exchange_square_valid(target)) {
        return false;
    }

    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto has_piece = [&board, attacker](int square, PieceType type) noexcept {
        if (!exchange_square_valid(square)) {
            return false;
        }
        const Piece piece = board[static_cast<std::size_t>(square)];
        return piece.color == attacker && piece.type == type;
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if ((target_file > 0 && has_piece((pawn_rank << 3) + target_file - 1, PieceType::pawn)) ||
            (target_file < 7 && has_piece((pawn_rank << 3) + target_file + 1, PieceType::pawn))) {
            return true;
        }
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8 &&
            has_piece((rank << 3) + file, PieceType::knight)) {
            return true;
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            if (has_piece((rank << 3) + file, PieceType::king)) {
                return true;
            }
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const Piece piece = board[static_cast<std::size_t>((rank << 3) + file)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    return true;
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
    return false;
}

template <typename CandidateHandler>
void for_each_exchange_attacker(const std::array<Piece, 64>& board, int target,
                                Color attacker, CandidateHandler&& handler) noexcept {
    if (!exchange_square_valid(target)) {
        return;
    }
    const int target_file = target & 7;
    const int target_rank = target >> 3;
    const auto visit = [&board, attacker, &handler](int square, PieceType expected) noexcept {
        if (exchange_square_valid(square) &&
            board[static_cast<std::size_t>(square)].color == attacker &&
            board[static_cast<std::size_t>(square)].type == expected) {
            handler(square);
        }
    };

    const int pawn_rank = target_rank - (attacker == Color::white ? 1 : -1);
    if (pawn_rank >= 0 && pawn_rank < 8) {
        if (target_file > 0) visit((pawn_rank << 3) + target_file - 1, PieceType::pawn);
        if (target_file < 7) visit((pawn_rank << 3) + target_file + 1, PieceType::pawn);
    }

    constexpr int knight_directions[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2},
    };
    for (const auto& direction : knight_directions) {
        const int file = target_file + direction[0];
        const int rank = target_rank + direction[1];
        if (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            visit((rank << 3) + file, PieceType::knight);
        }
    }

    for (int file = target_file - 1; file <= target_file + 1; ++file) {
        for (int rank = target_rank - 1; rank <= target_rank + 1; ++rank) {
            if ((file == target_file && rank == target_rank) ||
                file < 0 || file >= 8 || rank < 0 || rank >= 8) {
                continue;
            }
            visit((rank << 3) + file, PieceType::king);
        }
    }

    constexpr int directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1},
    };
    for (int direction = 0; direction < 8; ++direction) {
        const bool diagonal = direction >= 4;
        int file = target_file + directions[direction][0];
        int rank = target_rank + directions[direction][1];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const int square = (rank << 3) + file;
            const Piece piece = board[static_cast<std::size_t>(square)];
            if (!piece.empty()) {
                if (piece.color == attacker &&
                    ((diagonal && (piece.type == PieceType::bishop || piece.type == PieceType::queen)) ||
                     (!diagonal && (piece.type == PieceType::rook || piece.type == PieceType::queen)))) {
                    handler(square);
                }
                break;
            }
            file += directions[direction][0];
            rank += directions[direction][1];
        }
    }
}

inline bool exchange_recapture_is_legal(std::array<Piece, 64>& board, std::array<int, 2>& king_squares,
                                        Color side, int source, int target, Piece placed_piece) noexcept {
    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    const Piece captured_piece = board[static_cast<std::size_t>(target)];
    const std::size_t side_index = side == Color::white ? 0 : 1;
    const int saved_king_square = king_squares[side_index];

    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[side_index] = target;
    }
    const bool legal = !exchange_square_attacked_by(board, king_squares[side_index], opposite(side));
    board[static_cast<std::size_t>(source)] = moving_piece;
    board[static_cast<std::size_t>(target)] = captured_piece;
    king_squares[side_index] = saved_king_square;
    return legal;
}

// Lightweight board view for static exchange evaluation.  SEE only needs the
// mailbox layout, the king squares, and the side to move; building the full
// evaluation feature set (attack maps, pawn structure, phase, and so on) for
// every node that contains a capture was wasted work.
struct ExchangeContext {
    std::array<Piece, 64> board{};
    std::array<int, 2> king_squares{-1, -1};
    Color side_to_move = Color::white;
};

inline ExchangeContext exchange_context_from(const Position& position) noexcept {
    ExchangeContext context;
    position.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::uint64_t king = position.piece_bitboard(PieceType::king, color);
        if (king != 0) {
            context.king_squares[color == Color::white ? 0 : 1] =
                static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = position.side_to_move();
    return context;
}

// Same baseline through the public GameState surface, for search code that
// only holds a GameState.  GameState forwards copy_board_to, piece_bitboard,
// and side_to_move to the native Position, so this produces exactly the
// mailbox the Position overload above produces.
inline ExchangeContext exchange_context_from(const GameState& state) noexcept {
    ExchangeContext context;
    state.copy_board_to(context.board);
    for (const Color color : {Color::white, Color::black}) {
        const std::uint64_t king = state.piece_bitboard(PieceType::king, color);
        if (king != 0) {
            context.king_squares[color == Color::white ? 0 : 1] =
                static_cast<int>(std::countr_zero(king));
        }
    }
    context.side_to_move = state.side_to_move();
    return context;
}

inline int static_exchange_gain_from_features(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid ||
        (!initial.is_capture() && initial.captured_piece == PieceType::none)) {
        return 0;
    }

    std::array<Piece, 64> board = context.board;
    std::array<int, 2> king_squares = context.king_squares;
    const int source = move.from().index();
    const int target = move.to().index();
    if (!exchange_square_valid(source) || !exchange_square_valid(target)) {
        return 0;
    }

    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != context.side_to_move ||
        moving_piece.type != initial.moving_piece || king_squares[0] < 0 || king_squares[1] < 0) {
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
    std::array<int, kMaximumExchangeDepth> gains{};
    const Piece captured_piece = board[static_cast<std::size_t>(captured_square)];
    gains[0] = exchange_piece_value(captured_piece.type) + promotion_exchange_gain(move.promotion());

    Piece placed_piece = moving_piece;
    if (move.promotion() != Promotion::none) {
        placed_piece.type = promotion_piece_type(move.promotion());
    }
    board[static_cast<std::size_t>(source)] = {};
    board[static_cast<std::size_t>(captured_square)] = {};
    board[static_cast<std::size_t>(target)] = placed_piece;
    if (moving_piece.type == PieceType::king) {
        king_squares[moving_piece.color == Color::white ? 0 : 1] = target;
    }

    Color side = opposite(moving_piece.color);
    std::size_t depth = 0;
    for (;;) {
        int attacker_square = -1;
        int attacker_value = std::numeric_limits<int>::max();
        Piece recapturing_piece{};
        int recapture_promotion_gain = 0;

        for_each_exchange_attacker(board, target, side, [&](const int square) noexcept {
            const Piece candidate = board[static_cast<std::size_t>(square)];
            Piece replacement = candidate;
            int candidate_promotion_gain = 0;
            if (candidate.type == PieceType::pawn && (target >> 3 == 0 || target >> 3 == 7)) {
                replacement.type = PieceType::queen;
                candidate_promotion_gain =
                    exchange_piece_value(PieceType::queen) - exchange_piece_value(PieceType::pawn);
            }
            if (!exchange_recapture_is_legal(board, king_squares, side, square, target, replacement)) {
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

        const Piece target_piece = board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = exchange_piece_value(target_piece.type) - gains[depth - 1] +
            recapture_promotion_gain;

        const Piece departing_piece = board[static_cast<std::size_t>(attacker_square)];
        board[static_cast<std::size_t>(attacker_square)] = {};
        board[static_cast<std::size_t>(target)] = recapturing_piece;
        if (departing_piece.type == PieceType::king) {
            king_squares[side == Color::white ? 0 : 1] = target;
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
// of rebuilding the 64-square mailbox per capture; the score itself is
// identical to static_exchange_gain(GameState, MoveMetadata).
[[nodiscard]] inline int static_exchange_gain(const ExchangeContext& context,
                                              const MoveMetadata& initial) noexcept {
    return static_exchange_gain_from_features(context, initial);
}

} // namespace koi::detail
