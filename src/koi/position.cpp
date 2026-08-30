#include "koi/position.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <stdexcept>

namespace koi {

namespace {

bool split_fen_fields(std::string_view fen, std::array<std::string_view, 6>& fields) {
    std::size_t offset = 0;
    std::size_t count = 0;

    while (offset < fen.size()) {
        while (offset < fen.size() && std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        if (offset == fen.size()) {
            break;
        }
        if (count == fields.size()) {
            return false;
        }

        const std::size_t start = offset;
        while (offset < fen.size() && !std::isspace(static_cast<unsigned char>(fen[offset]))) {
            ++offset;
        }
        fields[count++] = fen.substr(start, offset - start);
    }

    return count == fields.size();
}

bool valid_piece_placement(std::string_view placement) {
    int rank_count = 1;
    int width = 0;
    int white_kings = 0;
    int black_kings = 0;
    int white_king_rank = -1;
    int white_king_file = -1;
    int black_king_rank = -1;
    int black_king_file = -1;

    for (const char character : placement) {
        if (character == '/') {
            if (width != 8 || rank_count == 8) {
                return false;
            }
            ++rank_count;
            width = 0;
        } else if (character >= '1' && character <= '8') {
            width += character - '0';
        } else if (std::string_view("PNBRQKpnbrqk").find(character) != std::string_view::npos) {
            if (character == 'K') {
                white_king_rank = rank_count - 1;
                white_king_file = width;
            } else if (character == 'k') {
                black_king_rank = rank_count - 1;
                black_king_file = width;
            }
            ++width;
            white_kings += character == 'K';
            black_kings += character == 'k';
        } else {
            return false;
        }

        if (width > 8) {
            return false;
        }
    }

    const int rank_distance = white_king_rank - black_king_rank;
    const int file_distance = white_king_file - black_king_file;
    const bool adjacent_kings = rank_distance >= -1 && rank_distance <= 1 && file_distance >= -1 &&
                                file_distance <= 1;

    return rank_count == 8 && width == 8 && white_kings == 1 && black_kings == 1 && !adjacent_kings;
}

bool valid_castling(std::string_view castling) {
    if (castling == "-") {
        return true;
    }

    if (castling.empty() || castling.size() > 4) {
        return false;
    }

    for (std::size_t index = 0; index < castling.size(); ++index) {
        if (std::string_view("KQkq").find(castling[index]) == std::string_view::npos ||
            castling.find(castling[index], index + 1) != std::string_view::npos) {
            return false;
        }
    }

    return true;
}

bool valid_en_passant(std::string_view en_passant, std::string_view side_to_move) {
    if (en_passant == "-") {
        return true;
    }

    if (en_passant.size() != 2 || en_passant[0] < 'a' || en_passant[0] > 'h') {
        return false;
    }

    return (side_to_move == "w" && en_passant[1] == '6') ||
           (side_to_move == "b" && en_passant[1] == '3');
}

bool valid_counter(std::string_view counter, bool allow_zero) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(counter.data(), counter.data() + counter.size(), value);
    return error == std::errc{} && end == counter.data() + counter.size() && (allow_zero || value > 0);
}

bool valid_fen(std::string_view fen) {
    std::array<std::string_view, 6> fields{};
    return split_fen_fields(fen, fields) && valid_piece_placement(fields[0]) &&
           (fields[1] == "w" || fields[1] == "b") && valid_castling(fields[2]) &&
           valid_en_passant(fields[3], fields[1]) && valid_counter(fields[4], true) &&
           valid_counter(fields[5], false);
}

} // namespace

Position::Position() = default;

Position::Position(std::string_view fen) {
    if (!set_fen(fen)) {
        throw std::invalid_argument("invalid FEN");
    }
}

std::string Position::fen() const {
    return board_.getFen();
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
    if (!chess::uci::isUciMove(uci)) {
        return false;
    }

    try {
        const chess::Move move = chess::uci::uciToMove(board_, uci);
        if (move.move() == chess::Move::NO_MOVE || !board_.isLegal(move)) {
            return false;
        }

        board_.makeMove(move);
        return true;
    } catch (...) {
        return false;
    }
}

bool Position::set_fen(std::string_view fen) {
    if (!valid_fen(fen)) {
        return false;
    }

    chess::Board candidate;
    if (!candidate.setFen(fen)) {
        return false;
    }

    board_ = std::move(candidate);
    return true;
}

const chess::Board& Position::board() const noexcept {
    return board_;
}

} // namespace koi
