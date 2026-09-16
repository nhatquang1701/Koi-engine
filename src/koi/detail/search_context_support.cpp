#include "koi/detail/search_context_support.hpp"

#include <algorithm>
#include <cmath>

#include "koi/detail/search_constants.hpp"
#include "koi/piece_values.hpp"

namespace koi::detail {

int piece_value(const PieceType type) noexcept {
    return piece_material_value(type);
}

bool piece_attacks_square(const PositionFeatures& features, const std::uint8_t source,
                          const std::uint8_t target) noexcept {
    if (source >= 64 || target >= 64 || source == target) {
        return false;
    }
    const Piece piece = features.board[source];
    if (piece.empty()) {
        return false;
    }

    const int source_file = source % 8;
    const int source_rank = source / 8;
    const int target_file = target % 8;
    const int target_rank = target / 8;
    const int file_delta = target_file - source_file;
    const int rank_delta = target_rank - source_rank;
    const int abs_file_delta = std::abs(file_delta);
    const int abs_rank_delta = std::abs(rank_delta);

    switch (piece.type) {
    case PieceType::pawn:
        return rank_delta == (piece.color == Color::white ? 1 : -1) && abs_file_delta == 1;
    case PieceType::knight:
        return (abs_file_delta == 1 && abs_rank_delta == 2) ||
               (abs_file_delta == 2 && abs_rank_delta == 1);
    case PieceType::king:
        return abs_file_delta <= 1 && abs_rank_delta <= 1;
    case PieceType::bishop:
    case PieceType::rook:
    case PieceType::queen:
        break;
    case PieceType::none:
        return false;
    }

    const bool diagonal = abs_file_delta == abs_rank_delta && abs_file_delta != 0;
    const bool orthogonal = (file_delta == 0) != (rank_delta == 0);
    if ((piece.type == PieceType::bishop && !diagonal) ||
        (piece.type == PieceType::rook && !orthogonal) ||
        (piece.type == PieceType::queen && !diagonal && !orthogonal)) {
        return false;
    }

    const int file_step = file_delta == 0 ? 0 : (file_delta > 0 ? 1 : -1);
    const int rank_step = rank_delta == 0 ? 0 : (rank_delta > 0 ? 1 : -1);
    for (int file = source_file + file_step, rank = source_rank + rank_step;
         file != target_file || rank != target_rank; file += file_step, rank += rank_step) {
        if (!features.board[static_cast<std::size_t>(rank * 8 + file)].empty()) {
            return false;
        }
    }
    return true;
}

bool quiet_move_is_forcing(const PositionFeatures& before,
                           const PositionFeatures& after_features,
                           const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const PositionFeatures& features = after_features;
    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    if (features.king_zone_attacks[enemy] > before.king_zone_attacks[enemy]) {
        return true;
    }
    const std::uint8_t destination = metadata.move.to().index();
    if (destination >= 64) {
        return false;
    }

    int attacked_valuable_pieces = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece target = features.board[square];
        if (target.empty() || target.type == PieceType::king ||
            (target.color == Color::white ? 0U : 1U) != enemy ||
            !piece_attacks_square(features, destination, square)) {
            continue;
        }
        if (target.type == PieceType::queen || target.type == PieceType::rook) {
            return true;
        }
        if (target.type == PieceType::pawn) {
            const int target_file = square % 8;
            const int target_rank = square / 8;
            if (target_file >= 2 && target_file <= 5 &&
                (target_rank == 3 || target_rank == 4)) {
                return true;
            }
        }
        if (++attacked_valuable_pieces >= 2) {
            return true;
        }
    }

    const std::uint64_t newly_attacked = features.attacked_squares[own] &
        ~before.attacked_squares[own];
    for (std::uint8_t square = 0; square < 64; ++square) {
        if ((newly_attacked & (std::uint64_t{1} << square)) == 0) {
            continue;
        }
        const Piece target = features.board[square];
        if (!target.empty() && (target.color == Color::white ? 0U : 1U) == enemy &&
            (target.type == PieceType::queen || target.type == PieceType::rook ||
             target.type == PieceType::bishop || target.type == PieceType::knight)) {
            return true;
        }
    }

    const Piece moved = features.board[destination];
    if (moved.type == PieceType::pawn && (moved.color == Color::white ? 0U : 1U) == own) {
        const int rank = destination / 8;
        const int file = destination % 8;
        const int direction = moved.color == Color::white ? 1 : -1;
        bool passed = true;
        for (int candidate_file = std::max(0, file - 1);
             candidate_file <= std::min(7, file + 1); ++candidate_file) {
            for (int candidate_rank = rank + direction;
                 candidate_rank >= 0 && candidate_rank < 8;
                 candidate_rank += direction) {
                const Piece candidate = features.board[
                    static_cast<std::size_t>(candidate_rank * 8 + candidate_file)];
                if (candidate.type == PieceType::pawn &&
                    (candidate.color == Color::white ? 0U : 1U) == enemy) {
                    passed = false;
                }
            }
        }
        const bool advanced = moved.color == Color::white ? rank >= 4 : rank <= 3;
        const bool central_break = (file == 3 || file == 4) &&
            (rank == 3 || rank == 4);
        if (passed && advanced) {
            return true;
        }
        if (central_break) {
            return true;
        }
    }
    return false;
}

bool quiet_move_has_direct_forcing_target(const PositionFeatures& before,
                                          const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const std::uint8_t source = metadata.move.from().index();
    const std::uint8_t destination = metadata.move.to().index();
    if (source >= 64 || destination >= 64 || source == destination) {
        return false;
    }

    const Piece moving = before.board[source];
    if (moving.empty()) {
        return false;
    }

    PositionFeatures projected = before;
    projected.board[source] = {};
    projected.board[destination] = moving;
    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    int attacked_minor_pieces = 0;
    for (std::uint8_t square = 0; square < 64; ++square) {
        const Piece target = projected.board[square];
        if (target.empty() || target.type == PieceType::king ||
            (target.color == Color::white ? 0U : 1U) != enemy ||
            !piece_attacks_square(projected, destination, square)) {
            continue;
        }
        if (target.type == PieceType::queen || target.type == PieceType::rook) {
            return true;
        }
        if (target.type == PieceType::pawn) {
            const int target_file = square % 8;
            const int target_rank = square / 8;
            if (target_file >= 2 && target_file <= 5 &&
                (target_rank == 3 || target_rank == 4)) {
                return true;
            }
        }
        if (target.type == PieceType::bishop || target.type == PieceType::knight) {
            ++attacked_minor_pieces;
            if (attacked_minor_pieces >= 2) {
                return true;
            }
        }
    }
    return false;
}

bool quiet_move_has_king_ring_target(const PositionFeatures& before,
                                     const MoveMetadata& metadata) {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const std::uint8_t source = metadata.move.from().index();
    const std::uint8_t destination = metadata.move.to().index();
    if (source >= 64 || destination >= 64 || source == destination) {
        return false;
    }

    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    const std::uint8_t enemy_king = before.king_squares[enemy].index();
    if (enemy_king >= 64) {
        return false;
    }

    const Piece moving = before.board[source];
    if (moving.empty()) {
        return false;
    }

    PositionFeatures projected = before;
    projected.board[source] = {};
    projected.board[destination] = moving;
    const int king_file = enemy_king % 8;
    const int king_rank = enemy_king / 8;
    for (int file = king_file - 1; file <= king_file + 1; ++file) {
        for (int rank = king_rank - 1; rank <= king_rank + 1; ++rank) {
            if (file < 0 || file >= 8 || rank < 0 || rank >= 8 ||
                (file == king_file && rank == king_rank)) {
                continue;
            }
            if (piece_attacks_square(projected, destination,
                                     static_cast<std::uint8_t>(rank * 8 + file))) {
                return true;
            }
        }
    }
    return false;
}

bool quiet_move_has_pawn_break_target(const MoveMetadata& metadata) noexcept {
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none ||
        metadata.moving_piece != PieceType::pawn) {
        return false;
    }

    const std::uint8_t destination = metadata.move.to().index();
    if (destination >= 64) {
        return false;
    }
    const int file = destination % 8;
    const int rank = destination / 8;
    return (file == 3 || file == 4) && (rank == 3 || rank == 4);
}

bool null_move_is_safe(const GameState& state, const PositionFeatures& features) noexcept {
    if (features.game_phase < 8 ||
        !state.has_non_pawn_material(state.side_to_move()) ||
        !state.has_non_pawn_material(opposite(state.side_to_move())) ||
        state.halfmove_clock() >= kNullMoveRuleSafetyHalfmoves ||
        state.is_repetition_sensitive()) {
        return false;
    }
    return state.tablebase_snapshot().piece_count() > kNullMoveSparsePieceLimit;
}

bool deep_quiet_check_candidate(const MoveMetadata& metadata) noexcept {
    return metadata.gives_check && !metadata.is_capture() &&
        metadata.move.promotion() == Promotion::none &&
        (metadata.moving_piece == PieceType::pawn ||
         metadata.moving_piece == PieceType::knight ||
         metadata.moving_piece == PieceType::bishop);
}

bool narrow_deep_quiet_check_candidate(GameState& state, const MoveMetadata& metadata) {
    if (deep_quiet_check_candidate(metadata)) {
        return true;
    }
    if (!metadata.gives_check || metadata.is_capture() ||
        metadata.move.promotion() != Promotion::none ||
        (metadata.moving_piece != PieceType::queen &&
         metadata.moving_piece != PieceType::rook)) {
        return false;
    }
    if (!state.make_search_move(metadata)) {
        return false;
    }
    const bool narrow_evasion_set = state.legal_moves().size() <= 3;
    (void)state.unmake_move();
    return narrow_evasion_set;
}

bool root_move_exposes_immediate_check(const GameState& root,
                                       const MoveMetadata& metadata) noexcept {
    try {
        GameState after_move = root;
        if (!after_move.make_search_move(metadata) || after_move.in_check()) {
            return false;
        }

        MoveMetadataList opponent_moves;
        after_move.legal_moves_with_metadata(
            opponent_moves, true, false, CheckFlagMode::all_moves);
        return std::any_of(opponent_moves.begin(), opponent_moves.end(),
                           [](const MoveMetadata& opponent_move) {
                               return opponent_move.gives_check;
                           });
    } catch (...) {
        return false;
    }
}

} // namespace koi::detail
