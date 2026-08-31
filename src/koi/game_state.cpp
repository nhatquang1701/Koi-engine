#include "koi/game_state.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <limits>
#include <utility>

#include <chess.hpp>

namespace koi {

namespace {

struct FenLayout {
    std::array<char, 64> squares{};
    int white_king_square = -1;
    int black_king_square = -1;
    int white_kings = 0;
    int black_kings = 0;
};

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

bool valid_piece_placement(std::string_view placement, FenLayout& layout) {
    layout = {};
    int rank = 7;
    int width = 0;
    for (char character : placement) {
        if (character == '/') {
            if (width != 8 || rank == 0) {
                return false;
            }
            --rank;
            width = 0;
        } else if (character >= '1' && character <= '8') {
            width += character - '0';
        } else if (std::string_view("PNBRQKpnbrqk").contains(character)) {
            if (width >= 8 || rank < 0) {
                return false;
            }
            const int square = rank * 8 + width;
            layout.squares[static_cast<std::size_t>(square)] = character;
            if (character == 'K') {
                layout.white_king_square = square;
                ++layout.white_kings;
            } else if (character == 'k') {
                layout.black_king_square = square;
                ++layout.black_kings;
            }
            ++width;
        } else {
            return false;
        }
        if (width > 8) {
            return false;
        }
    }

    const int white_rank = layout.white_king_square / 8;
    const int white_file = layout.white_king_square % 8;
    const int black_rank = layout.black_king_square / 8;
    const int black_file = layout.black_king_square % 8;
    const int rank_distance = white_rank - black_rank;
    const int file_distance = white_file - black_file;
    const bool adjacent_kings = rank_distance >= -1 && rank_distance <= 1 && file_distance >= -1 &&
                                file_distance <= 1;
    return rank == 0 && width == 8 && layout.white_kings == 1 && layout.black_kings == 1 && !adjacent_kings;
}

bool valid_castling(std::string_view castling, const FenLayout& layout) {
    if (castling == "-") {
        return true;
    }
    if (castling.empty() || castling.size() > 4) {
        return false;
    }
    for (std::size_t index = 0; index < castling.size(); ++index) {
        if (!std::string_view("KQkq").contains(castling[index]) ||
            castling.find(castling[index], index + 1) != std::string_view::npos) {
            return false;
        }
    }

    const auto piece_at = [&layout](int square) { return layout.squares[static_cast<std::size_t>(square)]; };
    return (!castling.contains('K') || (piece_at(4) == 'K' && piece_at(7) == 'R')) &&
           (!castling.contains('Q') || (piece_at(4) == 'K' && piece_at(0) == 'R')) &&
           (!castling.contains('k') || (piece_at(60) == 'k' && piece_at(63) == 'r')) &&
           (!castling.contains('q') || (piece_at(60) == 'k' && piece_at(56) == 'r'));
}

bool valid_en_passant(std::string_view en_passant, std::string_view side_to_move) {
    if (en_passant == "-") {
        return true;
    }
    return en_passant.size() == 2 && en_passant[0] >= 'a' && en_passant[0] <= 'h' &&
           ((side_to_move == "w" && en_passant[1] == '6') || (side_to_move == "b" && en_passant[1] == '3'));
}

bool valid_counter(std::string_view counter, bool allow_zero, std::uint32_t maximum) {
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(counter.data(), counter.data() + counter.size(), value);
    return error == std::errc{} && end == counter.data() + counter.size() && (allow_zero || value > 0) &&
           value <= maximum;
}

bool valid_fen_syntax(std::string_view fen) {
    std::array<std::string_view, 6> fields{};
    FenLayout layout;
    return split_fen_fields(fen, fields) && valid_piece_placement(fields[0], layout) &&
           (fields[1] == "w" || fields[1] == "b") && valid_castling(fields[2], layout) &&
           valid_en_passant(fields[3], fields[1]) && valid_counter(fields[4], true, 255) &&
           valid_counter(fields[5], false, 32768);
}

bool valid_check_counts(const chess::Board& board) {
    for (chess::Color color : {chess::Color::WHITE, chess::Color::BLACK}) {
        if (chess::attacks::attackers(board, ~color, board.kingSq(color)).count() > 2) {
            return false;
        }
    }
    return true;
}

Color koi_color(chess::Color color) {
    return color == chess::Color::WHITE ? Color::white : Color::black;
}

chess::Color native_color(Color color) {
    return color == Color::white ? chess::Color::WHITE : chess::Color::BLACK;
}

PieceType koi_piece_type(chess::PieceType type) {
    if (type == chess::PieceType::PAWN) return PieceType::pawn;
    if (type == chess::PieceType::KNIGHT) return PieceType::knight;
    if (type == chess::PieceType::BISHOP) return PieceType::bishop;
    if (type == chess::PieceType::ROOK) return PieceType::rook;
    if (type == chess::PieceType::QUEEN) return PieceType::queen;
    if (type == chess::PieceType::KING) return PieceType::king;
    return PieceType::none;
}

} // namespace

class GameState::Impl {
public:
    chess::Board board{};
    std::vector<chess::Move> history;
};

GameState::GameState() : impl_(std::make_unique<Impl>()) {}

GameState::GameState(const GameState& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

GameState::GameState(GameState&&) noexcept = default;

GameState& GameState::operator=(const GameState& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

GameState& GameState::operator=(GameState&&) noexcept = default;

GameState::~GameState() = default;

GameState GameState::startpos() {
    return {};
}

std::expected<GameState, PositionError> GameState::from_fen(std::string_view fen) {
    if (!valid_fen_syntax(fen)) {
        return std::unexpected(PositionError{PositionErrorCode::malformed_fen, "invalid FEN"});
    }

    chess::Board candidate;
    if (!candidate.setFen(fen) || !valid_check_counts(candidate)) {
        return std::unexpected(PositionError{PositionErrorCode::illegal_position, "illegal position"});
    }

    GameState state;
    state.impl_->board = std::move(candidate);
    return state;
}

std::string GameState::fen() const {
    return impl_->board.getFen();
}

Color GameState::side_to_move() const noexcept {
    return koi_color(impl_->board.sideToMove());
}

Piece GameState::piece_at(Square square) const noexcept {
    if (square.index() == Square::kInvalid) {
        return {};
    }
    const chess::Piece piece = impl_->board.at(chess::Square(square.index()));
    if (piece == chess::Piece::NONE) {
        return {};
    }
    return {koi_piece_type(piece.type()), koi_color(piece.color())};
}

std::vector<Move> GameState::legal_moves() const {
    chess::Movelist native_moves;
    chess::movegen::legalmoves(native_moves, impl_->board);

    std::vector<Move> moves;
    moves.reserve(static_cast<std::size_t>(native_moves.size()));
    for (chess::Move native_move : native_moves) {
        const auto move = Move::parse_uci(chess::uci::moveToUci(native_move));
        if (move) {
            moves.push_back(*move);
        }
    }
    return moves;
}

bool GameState::is_legal(const Move& move) const noexcept {
    if (move.is_no_move()) {
        return false;
    }
    try {
        const chess::Move native_move = chess::uci::uciToMove(impl_->board, move.uci());
        return native_move.move() != chess::Move::NO_MOVE && impl_->board.isLegal(native_move);
    } catch (...) {
        return false;
    }
}

bool GameState::make_move(const Move& move) noexcept {
    if (!is_legal(move)) {
        return false;
    }
    try {
        const chess::Move native_move = chess::uci::uciToMove(impl_->board, move.uci());
        impl_->board.makeMove(native_move);
        impl_->history.push_back(native_move);
        return true;
    } catch (...) {
        return false;
    }
}

bool GameState::unmake_move() noexcept {
    if (impl_->history.empty()) {
        return false;
    }
    impl_->board.unmakeMove(impl_->history.back());
    impl_->history.pop_back();
    return true;
}

bool GameState::is_capture(const Move& move) const noexcept {
    if (move.is_no_move()) {
        return false;
    }
    try {
        const chess::Move native_move = chess::uci::uciToMove(impl_->board, move.uci());
        return native_move.move() != chess::Move::NO_MOVE && impl_->board.isLegal(native_move) &&
               impl_->board.isCapture(native_move);
    } catch (...) {
        return false;
    }
}

bool GameState::in_check() const noexcept {
    return in_check(side_to_move());
}

bool GameState::in_check(Color color) const noexcept {
    const chess::Color native = native_color(color);
    return chess::attacks::attackers(impl_->board, ~native, impl_->board.kingSq(native)).count() != 0;
}

bool GameState::is_terminal() const noexcept {
    return impl_->board.isGameOver().second != chess::GameResult::NONE;
}

std::uint64_t GameState::position_key() const noexcept {
    return impl_->board.hash();
}

std::uint16_t GameState::halfmove_clock() const noexcept {
    return static_cast<std::uint16_t>(std::min<std::uint32_t>(impl_->board.halfMoveClock(),
                                                               std::numeric_limits<std::uint16_t>::max()));
}

} // namespace koi
