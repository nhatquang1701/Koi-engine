#include "koi/detail/compatibility_mirror.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace koi::detail {

// The native rule state clamps its halfmove clock here; the shadow board keeps
// the same counter in a byte and wraps to zero one ply later. Above this value
// the exact count no longer changes a rule decision, so comparisons accept the
// wrapped shadow value.
constexpr std::uint16_t kSaturatedHalfmoveClock = 255;

namespace {

constexpr std::uint8_t kWhiteKingSideCastling = 0x1;
constexpr std::uint8_t kWhiteQueenSideCastling = 0x2;
constexpr std::uint8_t kBlackKingSideCastling = 0x4;
constexpr std::uint8_t kBlackQueenSideCastling = 0x8;

[[nodiscard]] bool valid_check_counts(const chess::Board& board) {
    const bool white_in_check = chess::attacks::attackers(
        board, chess::Color::BLACK, board.kingSq(chess::Color::WHITE)).count() != 0;
    const bool black_in_check = chess::attacks::attackers(
        board, chess::Color::WHITE, board.kingSq(chess::Color::BLACK)).count() != 0;
    if (white_in_check && black_in_check) {
        return false;
    }
    for (const chess::Color color : {chess::Color::WHITE, chess::Color::BLACK}) {
        if (chess::attacks::attackers(board, ~color, board.kingSq(color)).count() > 2) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] chess::PieceType native_promotion_type(Promotion promotion) noexcept {
    switch (promotion) {
    case Promotion::knight:
        return chess::PieceType::KNIGHT;
    case Promotion::bishop:
        return chess::PieceType::BISHOP;
    case Promotion::rook:
        return chess::PieceType::ROOK;
    case Promotion::queen:
        return chess::PieceType::QUEEN;
    case Promotion::none:
        return chess::PieceType::NONE;
    }
    return chess::PieceType::NONE;
}

[[nodiscard]] std::uint8_t shadow_castling_rights(const chess::Board& board) noexcept {
    const chess::Board::CastlingRights rights = board.castlingRights();
    std::uint8_t result = 0;
    if (rights.has(chess::Color::WHITE, chess::Board::CastlingRights::Side::KING_SIDE)) {
        result |= kWhiteKingSideCastling;
    }
    if (rights.has(chess::Color::WHITE, chess::Board::CastlingRights::Side::QUEEN_SIDE)) {
        result |= kWhiteQueenSideCastling;
    }
    if (rights.has(chess::Color::BLACK, chess::Board::CastlingRights::Side::KING_SIDE)) {
        result |= kBlackKingSideCastling;
    }
    if (rights.has(chess::Color::BLACK, chess::Board::CastlingRights::Side::QUEEN_SIDE)) {
        result |= kBlackQueenSideCastling;
    }
    return result;
}

[[nodiscard]] Square shadow_en_passant_square(const chess::Board& board) noexcept {
    const chess::Square square = board.enpassantSq();
    if (square == chess::Square::NO_SQ) {
        return {};
    }
    return Square::from_index(static_cast<std::uint8_t>(square.index()));
}

[[nodiscard]] bool shadow_has_legal_en_passant_capture(const chess::Board& board) {
    chess::Movelist legal_moves;
    chess::movegen::legalmoves(legal_moves, board);
    for (const chess::Move move : legal_moves) {
        if (move.typeOf() == chess::Move::ENPASSANT && board.isLegal(move)) {
            return true;
        }
    }
    return false;
}

// Repetition identity must match the native key, which includes the
// en-passant file only while a legal en-passant capture exists. The vendored
// chess.hpp keeps the square whenever an enemy pawn attacks it, so a pinned
// (illegal) capturer would otherwise make the shadow count a different
// repetition than the native rule state. The rare divergent case is
// normalized by dropping the en-passant field before hashing.
[[nodiscard]] std::uint64_t shadow_repetition_key(const chess::Board& board) {
    if (board.enpassantSq() == chess::Square::NO_SQ ||
        shadow_has_legal_en_passant_capture(board)) {
        return board.hash();
    }
    try {
        std::string fen = board.getFen();
        const std::size_t board_end = fen.find(' ');
        const std::size_t side_end = fen.find(' ', board_end + 1);
        const std::size_t castling_end = fen.find(' ', side_end + 1);
        const std::size_t en_passant_end = fen.find(' ', castling_end + 1);
        if (board_end == std::string::npos || side_end == std::string::npos ||
            castling_end == std::string::npos || en_passant_end == std::string::npos) {
            return board.hash();
        }
        fen.replace(castling_end + 1, en_passant_end - castling_end - 1, "-");
        chess::Board probe;
        probe.setFen(fen);
        return probe.hash();
    } catch (...) {
        return board.hash();
    }
}

[[nodiscard]] std::string shadow_fen_for_native_comparison(const Position& native_position,
                                                           const chess::Board& shadow_board) {
    std::string fen = shadow_board.getFen();
    const std::size_t board_end = fen.find(' ');
    if (board_end == std::string::npos) return fen;
    const std::size_t side_end = fen.find(' ', board_end + 1);
    if (side_end == std::string::npos) return fen;
    const std::size_t castling_end = fen.find(' ', side_end + 1);
    if (castling_end == std::string::npos) return fen;
    const std::size_t en_passant_end = fen.find(' ', castling_end + 1);
    if (en_passant_end == std::string::npos) return fen;

    if (native_position.en_passant_square().index() >= Square::kInvalid &&
        shadow_board.enpassantSq() != chess::Square::NO_SQ &&
        !shadow_has_legal_en_passant_capture(shadow_board)) {
        fen.replace(castling_end + 1, en_passant_end - castling_end - 1, "-");
    }

    // The vendored shadow board keeps the halfmove clock in a byte, so a
    // replay past 255 reversible plies wraps it to zero while the native clock
    // clamps. Past the 75-move rule the exact value changes no rule decision,
    // so the comparison uses the native (authoritative) counter.
    const std::size_t shadow_en_passant_end = fen.find(' ', castling_end + 1);
    if (native_position.halfmove_clock() >= kSaturatedHalfmoveClock &&
        shadow_en_passant_end != std::string::npos) {
        const std::size_t shadow_halfmove_end = fen.find(' ', shadow_en_passant_end + 1);
        if (shadow_halfmove_end != std::string::npos) {
            fen.replace(shadow_en_passant_end + 1,
                        shadow_halfmove_end - shadow_en_passant_end - 1,
                        std::to_string(native_position.halfmove_clock()));
        }
    }
    return fen;
}

[[nodiscard]] Square shadow_en_passant_square_for_native_comparison(
    const Position& native_position, const chess::Board& shadow_board) {
    if (native_position.en_passant_square().index() < Square::kInvalid ||
        shadow_board.enpassantSq() == chess::Square::NO_SQ ||
        shadow_has_legal_en_passant_capture(shadow_board)) {
        return shadow_en_passant_square(shadow_board);
    }
    return {};
}

[[nodiscard]] std::vector<std::string> sorted_shadow_move_strings(const chess::Board& board) {
    chess::Movelist legal_moves;
    chess::movegen::legalmoves(legal_moves, board);
    std::vector<std::string> moves;
    moves.reserve(legal_moves.size());
    for (const chess::Move move : legal_moves) {
        if (board.isLegal(move)) {
            moves.push_back(chess::uci::moveToUci(move));
        }
    }
    std::sort(moves.begin(), moves.end());
    return moves;
}

} // namespace

bool CompatibilityMirror::set_fen(std::string_view fen) {
    try {
        chess::Board candidate;
        if (!candidate.setFen(fen) || !valid_check_counts(candidate)) {
            return false;
        }
        board_ = std::move(candidate);
        history_.clear();
        repetition_history_suppressed_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

chess::Move CompatibilityMirror::native_move_for(const Move& move) const noexcept {
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid) {
        return chess::Move{chess::Move::NO_MOVE};
    }

    const chess::Square source(move.from().index());
    const chess::Square target(move.to().index());
    const chess::Piece piece = board_.at(source);
    if (piece == chess::Piece::NONE) {
        return chess::Move{chess::Move::NO_MOVE};
    }

    if (piece.type() == chess::PieceType::KING && source.rank() == target.rank() &&
        std::abs(static_cast<int>(source.file()) - static_cast<int>(target.file())) == 2) {
        const chess::File rook_file = target > source ? chess::File::FILE_H : chess::File::FILE_A;
        return chess::Move::make<chess::Move::CASTLING>(
            source, chess::Square(rook_file, source.rank()));
    }

    if (piece.type() == chess::PieceType::PAWN && target == board_.enpassantSq()) {
        return chess::Move::make<chess::Move::ENPASSANT>(source, target);
    }

    if (move.promotion() != Promotion::none) {
        if (piece.type() != chess::PieceType::PAWN ||
            !chess::Square::back_rank(target, ~board_.sideToMove())) {
            return chess::Move{chess::Move::NO_MOVE};
        }
        return chess::Move::make<chess::Move::PROMOTION>(
            source, target, native_promotion_type(move.promotion()));
    }

    return chess::Move::make<chess::Move::NORMAL>(source, target);
}

chess::Move CompatibilityMirror::native_move_for(const MoveMetadata& metadata) const noexcept {
    const Move& move = metadata.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid) {
        return chess::Move{chess::Move::NO_MOVE};
    }

    const chess::Square source(move.from().index());
    const chess::Square target(move.to().index());
    switch (metadata.kind) {
    case MoveKind::castling: {
        const chess::File rook_file = target > source ? chess::File::FILE_H : chess::File::FILE_A;
        return chess::Move::make<chess::Move::CASTLING>(
            source, chess::Square(rook_file, source.rank()));
    }
    case MoveKind::en_passant:
        return chess::Move::make<chess::Move::ENPASSANT>(source, target);
    case MoveKind::promotion:
        if (metadata.moving_piece != PieceType::pawn) {
            return chess::Move{chess::Move::NO_MOVE};
        }
        return chess::Move::make<chess::Move::PROMOTION>(
            source, target, native_promotion_type(move.promotion()));
    case MoveKind::quiet:
    case MoveKind::capture:
        return chess::Move::make<chess::Move::NORMAL>(source, target);
    }
    return chess::Move{chess::Move::NO_MOVE};
}

bool CompatibilityMirror::apply_native(const chess::Move move, const bool null_move) noexcept {
    if (move.move() == chess::Move::NO_MOVE) {
        return false;
    }
    try {
        const std::uint8_t previous_castling_rights = shadow_castling_rights(board_);
        const chess::Piece moving_piece = null_move ? chess::Piece::NONE : board_.at(move.from());
        const bool pawn_move = !null_move && moving_piece.type() == chess::PieceType::PAWN;
        const bool capture = !null_move &&
            (move.typeOf() == chess::Move::ENPASSANT || board_.at(move.to()) != chess::Piece::NONE);
        if (history_.size() >= kMaximumMirrorHistory) {
            // Match the native snapshot window: a replay that runs past the
            // capacity drops the oldest record instead of rejecting the move,
            // so both sides count repetitions over the same plies.
            history_.erase(history_.begin());
        }
        history_.push_back(HistoryRecord{
            move, null_move, shadow_repetition_key(board_),
            repetition_history_suppressed_});
        if (null_move) {
            board_.makeNullMove();
        } else {
            board_.makeMove(move);
        }
        if (null_move) {
            repetition_history_suppressed_ = true;
        } else if (repetition_history_suppressed_ &&
                   (pawn_move || capture ||
                    previous_castling_rights != shadow_castling_rights(board_))) {
            // Match the native position: an irreversible move starts a fresh
            // real-history segment even when the move itself follows a
            // speculative null branch.
            repetition_history_suppressed_ = false;
        }
        return true;
    } catch (...) {
        if (!history_.empty() && history_.back().move == move &&
            history_.back().null_move == null_move) {
            history_.pop_back();
        }
        return false;
    }
}

bool CompatibilityMirror::apply_move(const Move& move) noexcept {
    const chess::Move native_move = native_move_for(move);
    if (native_move.move() == chess::Move::NO_MOVE || !board_.isLegal(native_move)) {
        return false;
    }
    return apply_native(native_move, false);
}

bool CompatibilityMirror::apply_generated_move(const MoveMetadata& metadata,
                                                const bool verify_legality) noexcept {
    const chess::Move native_move = native_move_for(metadata);
    if (native_move.move() == chess::Move::NO_MOVE ||
        (verify_legality && !board_.isLegal(native_move))) {
        return false;
    }
    return apply_native(native_move, false);
}

bool CompatibilityMirror::undo_move() noexcept {
    if (history_.empty() || history_.back().null_move) {
        return false;
    }
    const HistoryRecord record = history_.back();
    try {
        board_.unmakeMove(record.move);
    } catch (...) {
        return false;
    }
    history_.pop_back();
    repetition_history_suppressed_ = record.repetition_history_suppressed;
    return true;
}

bool CompatibilityMirror::apply_null_move() noexcept {
    return apply_native(chess::Move{chess::Move::NULL_MOVE}, true);
}

bool CompatibilityMirror::undo_null_move() noexcept {
    if (history_.empty() || !history_.back().null_move) {
        return false;
    }
    const HistoryRecord record = history_.back();
    try {
        board_.unmakeNullMove();
    } catch (...) {
        return false;
    }
    history_.pop_back();
    repetition_history_suppressed_ = record.repetition_history_suppressed;
    return true;
}

bool CompatibilityMirror::last_move_is_null() const noexcept {
    return !history_.empty() && history_.back().null_move;
}

std::size_t CompatibilityMirror::history_size() const noexcept {
    return history_.size();
}

std::uint64_t CompatibilityMirror::position_key() const noexcept {
    return board_.hash();
}

std::uint8_t CompatibilityMirror::castling_rights() const noexcept {
    return shadow_castling_rights(board_);
}

Square CompatibilityMirror::en_passant_square() const noexcept {
    return shadow_en_passant_square(board_);
}

std::uint16_t CompatibilityMirror::halfmove_clock() const noexcept {
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(
        board_.halfMoveClock(), std::numeric_limits<std::uint16_t>::max()));
}

std::uint16_t CompatibilityMirror::fullmove_number() const noexcept {
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(
        board_.fullMoveNumber(), std::numeric_limits<std::uint16_t>::max()));
}

bool CompatibilityMirror::in_check() const noexcept {
    return board_.inCheck();
}

Color CompatibilityMirror::side_to_move() const noexcept {
    return board_.sideToMove() == chess::Color::WHITE ? Color::white : Color::black;
}

std::size_t CompatibilityMirror::repetition_count() const noexcept {
    if (repetition_history_suppressed_) {
        return 1;
    }
    const std::uint64_t key = shadow_repetition_key(board_);
    std::size_t count = 1;
    for (auto record = history_.rbegin(); record != history_.rend(); ++record) {
        if (record->null_move) {
            break;
        }
        if (record->repetition_key == key) {
            ++count;
        }
    }
    return count;
}

std::vector<std::string> CompatibilityMirror::legal_move_strings() const {
    return sorted_shadow_move_strings(board_);
}

std::string CompatibilityMirror::fen_for_comparison(const Position& native) const {
    return shadow_fen_for_native_comparison(native, board_);
}

Square CompatibilityMirror::en_passant_square_for_comparison(const Position& native) const {
    return shadow_en_passant_square_for_native_comparison(native, board_);
}

bool CompatibilityMirror::matches(const Position& native) const {
    // The shadow's byte-sized halfmove clock wraps once the native clock is
    // saturated; above the 75-move rule the exact value changes no decision.
    const bool halfmove_agrees =
        native.halfmove_clock() == halfmove_clock() ||
        native.halfmove_clock() >= kSaturatedHalfmoveClock;
    if (native.side_to_move() != side_to_move() ||
        native.castling_rights() != castling_rights() ||
        !halfmove_agrees ||
        native.fullmove_number() != fullmove_number()) {
        return false;
    }

    constexpr std::array<std::pair<PieceType, chess::PieceType>, 6> piece_types{{
        {PieceType::pawn, chess::PieceType::PAWN},
        {PieceType::knight, chess::PieceType::KNIGHT},
        {PieceType::bishop, chess::PieceType::BISHOP},
        {PieceType::rook, chess::PieceType::ROOK},
        {PieceType::queen, chess::PieceType::QUEEN},
        {PieceType::king, chess::PieceType::KING},
    }};
    for (const Color color : {Color::white, Color::black}) {
        const chess::Color shadow_color = color == Color::white ?
            chess::Color::WHITE : chess::Color::BLACK;
        for (const auto [native_type, shadow_type] : piece_types) {
            if (native.piece_bitboard(native_type, color) !=
                board_.pieces(shadow_type, shadow_color).getBits()) {
                return false;
            }
        }
    }

    if (native.piece_count() != board_.occ().count()) {
        return false;
    }

    const Square native_en_passant = native.en_passant_square();
    const Square shadow_en_passant = en_passant_square();
    if (native_en_passant.index() < Square::kInvalid) {
        return shadow_en_passant == native_en_passant;
    }
    return shadow_en_passant.index() >= Square::kInvalid ||
        !shadow_has_legal_en_passant_capture(board_);
}

bool CompatibilityMirror::gives_check(const Move& move) const noexcept {
    const chess::Move native_move = native_move_for(move);
    return native_move.move() != chess::Move::NO_MOVE &&
        board_.givesCheck(native_move) != chess::CheckType::NO_CHECK;
}

} // namespace koi::detail
