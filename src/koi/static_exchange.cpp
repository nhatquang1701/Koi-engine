#include "koi/detail/static_exchange.hpp"

#include <algorithm>
#include <array>

namespace koi::detail {
namespace {

constexpr int piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return 100;
    case PieceType::knight:
        return 320;
    case PieceType::bishop:
        return 330;
    case PieceType::rook:
        return 500;
    case PieceType::queen:
        return 900;
    case PieceType::king:
        return 20'000;
    case PieceType::none:
        return 0;
    }
    return 0;
}

int promotion_gain(Promotion promotion) noexcept {
    return piece_value(promotion == Promotion::knight ? PieceType::knight :
                       promotion == Promotion::bishop ? PieceType::bishop :
                       promotion == Promotion::rook ? PieceType::rook :
                       promotion == Promotion::queen ? PieceType::queen : PieceType::pawn) -
        piece_value(PieceType::pawn);
}

} // namespace

int static_exchange_gain(const GameState& state, const MoveMetadata& initial) noexcept {
    try {
        if (!initial.is_capture()) {
            return 0;
        }

        constexpr std::size_t kMaximumExchangeDepth = 32;
        std::array<int, kMaximumExchangeDepth> gains{};
        gains[0] = piece_value(initial.captured_piece) + promotion_gain(initial.move.promotion());

        GameState position = state;
        if (!position.make_legal_move(initial)) {
            return 0;
        }

        const Square target = initial.move.to();
        std::size_t depth = 0;
        for (;;) {
            MoveMetadata least_valuable_attacker;
            int least_value = 100'000;
            MoveMetadataList candidates;
            position.legal_moves_with_metadata(candidates);
            for (const MoveMetadata& candidate : candidates) {
                if (candidate.move.to() != target || !candidate.is_capture()) {
                    continue;
                }
                const int attacker_value = piece_value(candidate.moving_piece);
                if (attacker_value < least_value) {
                    least_value = attacker_value;
                    least_valuable_attacker = candidate;
                }
            }
            if (least_valuable_attacker.move.is_no_move() || depth + 1 >= kMaximumExchangeDepth) {
                break;
            }

            const int captured_value = piece_value(position.piece_at(target).type);
            ++depth;
            gains[depth] = captured_value - gains[depth - 1] +
                promotion_gain(least_valuable_attacker.move.promotion());
            if (!position.make_legal_move(least_valuable_attacker)) {
                --depth;
                break;
            }
        }

        while (depth > 0) {
            gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
            --depth;
        }
        return gains[0];
    } catch (...) {
        return 0;
    }
}

int static_exchange_gain(const GameState& state, Move move) noexcept {
    const auto metadata = state.describe_move(move);
    return metadata.has_value() ? static_exchange_gain(state, *metadata) : 0;
}

} // namespace koi::detail
