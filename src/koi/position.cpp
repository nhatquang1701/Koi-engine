#include "koi/position.hpp"

#include <stdexcept>
#include <string>

namespace koi {

namespace {

std::string with_black_double_pawn_target(const chess::Board& board, const chess::Move& move) {
    const std::string uci = chess::uci::moveToUci(move);
    const std::string target{uci[2], static_cast<char>((uci[1] + uci[3]) / 2)};
    std::string fen = board.getFen();

    const std::size_t en_passant_begin = fen.find(' ', fen.find(' ', fen.find(' ') + 1) + 1) + 1;
    const std::size_t en_passant_end = fen.find(' ', en_passant_begin);
    fen.replace(en_passant_begin, en_passant_end - en_passant_begin, target);
    return fen;
}

} // namespace

Position::Position() = default;

Position::Position(std::string_view fen) {
    if (!set_fen(fen)) {
        throw std::invalid_argument("invalid FEN");
    }
}

std::string Position::fen() const {
    return fen_override_.value_or(board_.getFen());
}

std::vector<Move> Position::legal_moves() const {
    chess::Movelist moves;
    chess::movegen::legalmoves(moves, board_);

    std::vector<Move> result;
    result.reserve(static_cast<std::size_t>(moves.size()));
    for (const chess::Move move : moves) {
        result.emplace_back(move);
    }
    return result;
}

bool Position::apply_uci(std::string_view uci) {
    try {
        const chess::Move move = chess::uci::uciToMove(board_, uci);
        if (move.move() == chess::Move::NO_MOVE || !board_.isLegal(move)) {
            return false;
        }

        const bool is_black_double_pawn_push =
            board_.sideToMove() == chess::Color::BLACK &&
            board_.at(move.from()).type() == chess::PieceType::PAWN &&
            move.from().file() == move.to().file() &&
            move.from().index() - move.to().index() == 16;
        board_.makeMove(move);
        fen_override_.reset();
        if (is_black_double_pawn_push) {
            fen_override_ = with_black_double_pawn_target(board_, move);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool Position::set_fen(std::string_view fen) {
    chess::Board candidate;
    if (!candidate.setFen(fen)) {
        return false;
    }

    board_ = std::move(candidate);
    fen_override_.reset();
    return true;
}

const chess::Board& Position::board() const noexcept {
    return board_;
}

} // namespace koi
