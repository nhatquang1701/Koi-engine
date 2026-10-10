#include "koi/detail/search_context_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include "koi/detail/attack_tables.hpp"
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
    if (features.occupied != 0) {
        // Bitboard path.  The moved piece's attack mask with post-move
        // occupancy is exactly the set of squares piece_attacks_square would
        // accept: the tables encode the same geometry and first-blocker
        // clearance the board walk applies.  Intersecting with `colors[enemy]`
        // keeps only enemy-occupied squares, and ascending set-bit iteration
        // (countr_zero) visits them in the same order as the 64-square scan.
        std::uint64_t attacks = 0;
        const Piece moved = features.board[destination];
        switch (moved.type) {
        case PieceType::pawn:
            attacks = pawn_attacks(destination, moved.color == Color::white);
            break;
        case PieceType::knight:
            attacks = knight_attacks(destination);
            break;
        case PieceType::king:
            attacks = king_attacks(destination);
            break;
        case PieceType::bishop:
            attacks = bishop_attacks(destination, features.occupied);
            break;
        case PieceType::rook:
            attacks = rook_attacks(destination, features.occupied);
            break;
        case PieceType::queen:
            attacks = queen_attacks(destination, features.occupied);
            break;
        case PieceType::none:
            break;
        }
        attacks &= features.colors[enemy];
        while (attacks != 0) {
            const std::uint8_t square =
                static_cast<std::uint8_t>(std::countr_zero(attacks));
            attacks &= attacks - 1;
            const Piece target = features.board[square];
            if (target.empty() || target.type == PieceType::king ||
                (target.color == Color::white ? 0U : 1U) != enemy) {
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
    } else {
        // Synthetic fixtures carry no bitboards; keep the board-scanning path.
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
    }

    std::uint64_t newly_attacked = features.attacked_squares[own] &
        ~before.attacked_squares[own];
    while (newly_attacked != 0) {
        const std::uint8_t square =
            static_cast<std::uint8_t>(std::countr_zero(newly_attacked));
        newly_attacked &= newly_attacked - 1;
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

namespace {

// Byte-exact copy of game_state.cpp's file-local king_zone_mask().  That
// helper is not exported and this change does not own game_state.cpp, so the
// 3x3 file/rank clipping (and the out-of-range guard) is duplicated verbatim
// to keep the compact king-zone count identical to
// PositionFeatures::king_zone_attacks[enemy].
[[nodiscard]] std::uint64_t compact_king_zone_mask(const std::uint8_t square) noexcept {
    if (square >= Square::kInvalid) {
        return 0;
    }
    const int file = square % 8;
    const int rank = square / 8;
    std::uint64_t mask = 0;
    for (int rank_delta = -1; rank_delta <= 1; ++rank_delta) {
        for (int file_delta = -1; file_delta <= 1; ++file_delta) {
            const int target_file = file + file_delta;
            const int target_rank = rank + rank_delta;
            if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                mask |= std::uint64_t{1} << (target_rank * 8 + target_file);
            }
        }
    }
    return mask;
}

// Mirror of a single native_feature_attacks() lookup (game_state.cpp): the
// same precomputed attack tables with the same first-blocker occupancy
// semantics for sliders.
[[nodiscard]] std::uint64_t compact_feature_attacks(
    const int square, const PieceType type, const Color color,
    const std::uint64_t occupied) noexcept {
    switch (type) {
    case PieceType::pawn:
        return pawn_attacks(square, color == Color::white);
    case PieceType::knight:
        return knight_attacks(square);
    case PieceType::king:
        return king_attacks(square);
    case PieceType::bishop:
        return bishop_attacks(square, occupied);
    case PieceType::rook:
        return rook_attacks(square, occupied);
    case PieceType::queen:
        return queen_attacks(square, occupied);
    case PieceType::none:
        return 0;
    }
    return 0;
}

} // namespace

void build_quiet_forcing_after_view(const GameState& after, const Color mover,
                                    QuietForcingAfterView& out) noexcept {
    // Type order matches native_position_features(); the union is
    // order-independent and every mask comes from the same attack tables.
    constexpr std::array<PieceType, 6> kAttackOrder{
        PieceType::pawn, PieceType::knight, PieceType::bishop,
        PieceType::rook, PieceType::queen, PieceType::king};
    const Color enemy = opposite(mover);
    out = QuietForcingAfterView{};

    std::array<std::uint64_t, kAttackOrder.size()> mover_boards{};
    std::uint64_t own_pieces = 0;
    std::uint64_t enemy_pieces = 0;
    for (std::size_t index = 0; index < kAttackOrder.size(); ++index) {
        mover_boards[index] = after.piece_bitboard(kAttackOrder[index], mover);
        own_pieces |= mover_boards[index];
        enemy_pieces |= after.piece_bitboard(kAttackOrder[index], enemy);
    }
    out.occupied = own_pieces | enemy_pieces;
    out.enemy_pieces = enemy_pieces;
    out.enemy_pawns = after.piece_bitboard(PieceType::pawn, enemy);

    // attacks_after[own]: native_position_features() builds the same union
    // from these same maintained bitboards with the post-move occupancy
    // (apply_unchecked refreshes state.occupied at position.cpp:1705 before
    // the search observes the child).  Per-piece attack sets are identical
    // and OR is order-independent, so the unions are bit-equal.
    std::uint64_t own_attacks = 0;
    for (std::size_t index = 0; index < kAttackOrder.size(); ++index) {
        std::uint64_t remaining = mover_boards[index];
        while (remaining != 0) {
            const int square = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            own_attacks |= compact_feature_attacks(
                square, kAttackOrder[index], mover, out.occupied);
        }
    }
    out.own_attacked_squares = own_attacks;

    // after.king_zone_attacks[enemy] == popcount(attacks_after[own] &
    // king_zone_mask(enemy king square)); the full extraction computes exactly
    // this expression from the enemy king bitboard (kInvalid when absent).
    const std::uint64_t enemy_king = after.piece_bitboard(PieceType::king, enemy);
    const std::uint8_t enemy_king_square = enemy_king == 0
        ? Square::kInvalid
        : static_cast<std::uint8_t>(std::countr_zero(enemy_king));
    out.enemy_king_zone_attacks = static_cast<std::uint8_t>(
        std::popcount(own_attacks & compact_king_zone_mask(enemy_king_square)));
}

bool quiet_move_is_forcing(const PositionFeatures& before, const GameState& after_state,
                           const QuietForcingAfterView& after_view,
                           const MoveMetadata& metadata) {
    // Bit-identity with quiet_move_is_forcing(before, after_features, metadata):
    //  1. after_view.own_attacked_squares equals
    //     after_features.attacked_squares[own] (same tables, same post-move
    //     occupancy).
    //  2. after_view.enemy_king_zone_attacks equals
    //     after_features.king_zone_attacks[enemy].
    //  3. the metadata guard, king-zone comparison, attacked-piece scan,
    //     newly-attacked scan, and passed-pawn branch keep the original check
    //     order, and both bitboard scans iterate squares in ascending
    //     countr_zero order over the same square sets (board reads come from
    //     the same native position the full snapshot is extracted from).
    //  4. the passed-pawn branch is the bitboard form of the original board
    //     scan: enemy_pawns & adjacent_files(file) & ranks strictly ahead.
    //  5. before.attacked_squares[own] and before.king_zone_attacks[enemy]
    //     still come from the parent snapshot passed by the caller.
    //  6. the caller feeds the result only into lmr_gate/dynamic_late_move,
    //     which consume the same booleans and produce the same reduction.
    //  7. no TT key, move ordering, history update, or emission order is
    //     touched here.
    if (metadata.is_capture() || metadata.gives_check ||
        metadata.move.promotion() != Promotion::none) {
        return false;
    }

    const std::size_t own = before.side_to_move == Color::white ? 0U : 1U;
    const std::size_t enemy = 1U - own;
    if (after_view.occupied == 0) {
        // The view was not built (or the native position has no pieces).  Use
        // the full-features decision, including its board-scanning path, so
        // hand-built fixtures keep their exact previous behavior.
        return quiet_move_is_forcing(before, after_state.position_features(), metadata);
    }
    if (after_view.enemy_king_zone_attacks > before.king_zone_attacks[enemy]) {
        return true;
    }
    const std::uint8_t destination = metadata.move.to().index();
    if (destination >= 64) {
        return false;
    }

    int attacked_valuable_pieces = 0;
    const Piece moved = after_state.piece_at(Square::from_index(destination));
    std::uint64_t attacks = 0;
    switch (moved.type) {
    case PieceType::pawn:
        attacks = pawn_attacks(destination, moved.color == Color::white);
        break;
    case PieceType::knight:
        attacks = knight_attacks(destination);
        break;
    case PieceType::king:
        attacks = king_attacks(destination);
        break;
    case PieceType::bishop:
        attacks = bishop_attacks(destination, after_view.occupied);
        break;
    case PieceType::rook:
        attacks = rook_attacks(destination, after_view.occupied);
        break;
    case PieceType::queen:
        attacks = queen_attacks(destination, after_view.occupied);
        break;
    case PieceType::none:
        break;
    }
    attacks &= after_view.enemy_pieces;
    while (attacks != 0) {
        const std::uint8_t square =
            static_cast<std::uint8_t>(std::countr_zero(attacks));
        attacks &= attacks - 1;
        const Piece target = after_state.piece_at(Square::from_index(square));
        if (target.empty() || target.type == PieceType::king ||
            (target.color == Color::white ? 0U : 1U) != enemy) {
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

    std::uint64_t newly_attacked = after_view.own_attacked_squares &
        ~before.attacked_squares[own];
    while (newly_attacked != 0) {
        const std::uint8_t square =
            static_cast<std::uint8_t>(std::countr_zero(newly_attacked));
        newly_attacked &= newly_attacked - 1;
        const Piece target = after_state.piece_at(Square::from_index(square));
        if (!target.empty() && (target.color == Color::white ? 0U : 1U) == enemy &&
            (target.type == PieceType::queen || target.type == PieceType::rook ||
             target.type == PieceType::bishop || target.type == PieceType::knight)) {
            return true;
        }
    }

    if (moved.type == PieceType::pawn && (moved.color == Color::white ? 0U : 1U) == own) {
        const int rank = destination / 8;
        const int file = destination % 8;
        const int direction = moved.color == Color::white ? 1 : -1;
        // The original scan walks the destination file and its neighbours
        // strictly ahead of the pawn; the enemy pawn bitboard intersected with
        // that same square set is equivalent to finding an enemy pawn there.
        std::uint64_t ahead_mask = 0;
        for (int candidate_file = std::max(0, file - 1);
             candidate_file <= std::min(7, file + 1); ++candidate_file) {
            for (int candidate_rank = rank + direction;
                 candidate_rank >= 0 && candidate_rank < 8;
                 candidate_rank += direction) {
                ahead_mask |= std::uint64_t{1} << (candidate_rank * 8 + candidate_file);
            }
        }
        const bool passed = (after_view.enemy_pawns & ahead_mask) == 0;
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

bool null_move_is_safe(const GameState& state, const int phase) noexcept {
    // No repetition re-check: the only caller reaches this function only when
    // SearchPolicy::dynamic_null_move() reported eligible, which requires
    // `null_move_allowed`; that flag already contains
    // `!repetition_sensitive` (search_context.cpp), so the direct
    // state.is_repetition_sensitive() scan could only return false here.
    if (phase < 8 ||
        !state.has_non_pawn_material(state.side_to_move()) ||
        !state.has_non_pawn_material(opposite(state.side_to_move())) ||
        state.halfmove_clock() >= kNullMoveRuleSafetyHalfmoves) {
        return false;
    }
    // GameState::piece_count() forwards to the native position's maintained
    // bitboards, which are the same per-piece bitboards the tablebase snapshot
    // is rebuilt from (kings included), so the sparse gate is unchanged.
    return state.piece_count() > kNullMoveSparsePieceLimit;
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
    std::array<Move, 4> narrow_replies{};
    const bool narrow_evasion_set = state.legal_moves_into(narrow_replies) <= 3;
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
