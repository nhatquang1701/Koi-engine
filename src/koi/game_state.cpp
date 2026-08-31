#include "koi/game_state.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdlib>
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
            if ((character == 'P' || character == 'p') && (rank == 0 || rank == 7)) {
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

bool valid_en_passant(std::string_view en_passant, std::string_view side_to_move,
                      std::string_view halfmove_clock, const FenLayout& layout) {
    if (en_passant == "-") {
        return true;
    }
    if (en_passant.size() != 2 || en_passant[0] < 'a' || en_passant[0] > 'h' ||
        !((side_to_move == "w" && en_passant[1] == '6') || (side_to_move == "b" && en_passant[1] == '3')) ||
        halfmove_clock != "0") {
        return false;
    }

    const int target_file = en_passant[0] - 'a';
    const int target_rank = en_passant[1] - '1';
    const int target_square = target_rank * 8 + target_file;
    const int pawn_square = (side_to_move == "w" ? target_rank - 1 : target_rank + 1) * 8 + target_file;
    const int origin_square = (side_to_move == "w" ? target_rank + 1 : target_rank - 1) * 8 + target_file;
    const char pawn = side_to_move == "w" ? 'p' : 'P';
    return layout.squares[static_cast<std::size_t>(target_square)] == '\0' &&
           layout.squares[static_cast<std::size_t>(pawn_square)] == pawn &&
           layout.squares[static_cast<std::size_t>(origin_square)] == '\0';
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
           valid_en_passant(fields[3], fields[1], fields[4], layout) && valid_counter(fields[4], true, 255) &&
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

chess::PieceType native_promotion_type(Promotion promotion) noexcept {
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

Promotion koi_promotion_type(chess::PieceType promotion) noexcept {
    if (promotion == chess::PieceType::KNIGHT) return Promotion::knight;
    if (promotion == chess::PieceType::BISHOP) return Promotion::bishop;
    if (promotion == chess::PieceType::ROOK) return Promotion::rook;
    if (promotion == chess::PieceType::QUEEN) return Promotion::queen;
    return Promotion::none;
}

chess::Move native_move_for(const chess::Board& board, const Move& move) noexcept {
    if (move.is_no_move() || move.from().index() == Square::kInvalid ||
        move.to().index() == Square::kInvalid) {
        return chess::Move{chess::Move::NO_MOVE};
    }

    const chess::Square source(move.from().index());
    const chess::Square target(move.to().index());
    const chess::Piece piece = board.at(source);
    if (piece == chess::Piece::NONE) {
        return chess::Move{chess::Move::NO_MOVE};
    }

    if (piece.type() == chess::PieceType::KING && source.rank() == target.rank() &&
        std::abs(static_cast<int>(source.file()) - static_cast<int>(target.file())) == 2) {
        const chess::File rook_file = target > source ? chess::File::FILE_H : chess::File::FILE_A;
        return chess::Move::make<chess::Move::CASTLING>(source, chess::Square(rook_file, source.rank()));
    }

    if (piece.type() == chess::PieceType::PAWN && target == board.enpassantSq()) {
        return chess::Move::make<chess::Move::ENPASSANT>(source, target);
    }

    if (move.promotion() != Promotion::none) {
        if (piece.type() != chess::PieceType::PAWN ||
            !chess::Square::back_rank(target, ~board.sideToMove())) {
            return chess::Move{chess::Move::NO_MOVE};
        }
        return chess::Move::make<chess::Move::PROMOTION>(source, target,
                                                         native_promotion_type(move.promotion()));
    }

    return chess::Move::make<chess::Move::NORMAL>(source, target);
}

chess::Move native_move_for_metadata(const MoveMetadata& metadata) noexcept {
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
        return chess::Move::make<chess::Move::CASTLING>(source,
                                                        chess::Square(rook_file, source.rank()));
    }
    case MoveKind::en_passant:
        return chess::Move::make<chess::Move::ENPASSANT>(source, target);
    case MoveKind::promotion:
        if (metadata.moving_piece != PieceType::pawn) {
            return chess::Move{chess::Move::NO_MOVE};
        }
        return chess::Move::make<chess::Move::PROMOTION>(source, target,
                                                         native_promotion_type(move.promotion()));
    case MoveKind::quiet:
    case MoveKind::capture:
        return chess::Move::make<chess::Move::NORMAL>(source, target);
    }
    return chess::Move{chess::Move::NO_MOVE};
}

Move koi_move_for(chess::Move move) noexcept {
    if (move.move() == chess::Move::NO_MOVE) {
        return Move::no_move();
    }

    const Square from = Square::from_index(move.from().index());
    Square to = Square::from_index(move.to().index());
    Promotion promotion = Promotion::none;
    if (move.typeOf() == chess::Move::CASTLING) {
        const std::uint8_t file = move.to() > move.from() ? 6 : 2;
        to = Square::from_index(static_cast<std::uint8_t>(move.from().rank() * 8 + file));
    } else if (move.typeOf() == chess::Move::PROMOTION) {
        promotion = koi_promotion_type(move.promotionType());
    }
    return Move(from, to, promotion);
}

MoveMetadata metadata_for_native_move(const chess::Board& board, chess::Move native_move,
                                      Move move, bool gives_check) noexcept {
    const chess::Piece moving_piece = board.at(native_move.from());
    MoveMetadata metadata;
    metadata.move = move;
    metadata.moving_piece = koi_piece_type(moving_piece.type());
    metadata.captured_piece = koi_piece_type(board.getCapturing<chess::PieceType>(native_move));
    if (native_move.typeOf() == chess::Move::CASTLING) {
        metadata.kind = MoveKind::castling;
    } else if (native_move.typeOf() == chess::Move::ENPASSANT) {
        metadata.kind = MoveKind::en_passant;
    } else if (native_move.typeOf() == chess::Move::PROMOTION) {
        metadata.kind = MoveKind::promotion;
    } else if (board.isCapture(native_move)) {
        metadata.kind = MoveKind::capture;
    }
    metadata.gives_check = gives_check;
    return metadata;
}

constexpr int exchange_piece_value(PieceType type) noexcept {
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

bool exchange_piece_attacks_square(const std::array<Piece, 64>& board, int source, int target) noexcept {
    const Piece piece = board[static_cast<std::size_t>(source)];
    if (piece.empty() || source == target) {
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

    if (piece.type == PieceType::pawn) {
        const int direction = piece.color == Color::white ? 1 : -1;
        return rank_delta == direction && abs_file_delta == 1;
    }
    if (piece.type == PieceType::knight) {
        return (abs_file_delta == 1 && abs_rank_delta == 2) ||
               (abs_file_delta == 2 && abs_rank_delta == 1);
    }
    if (piece.type == PieceType::king) {
        return abs_file_delta <= 1 && abs_rank_delta <= 1;
    }

    const bool diagonal = abs_file_delta == abs_rank_delta;
    const bool orthogonal = file_delta == 0 || rank_delta == 0;
    if ((piece.type == PieceType::bishop && !diagonal) ||
        (piece.type == PieceType::rook && !orthogonal) ||
        (piece.type == PieceType::queen && !diagonal && !orthogonal) ||
        (piece.type != PieceType::bishop && piece.type != PieceType::rook && piece.type != PieceType::queen)) {
        return false;
    }

    const int file_step = file_delta == 0 ? 0 : (file_delta > 0 ? 1 : -1);
    const int rank_step = rank_delta == 0 ? 0 : (rank_delta > 0 ? 1 : -1);
    for (int file = source_file + file_step, rank = source_rank + rank_step;
         file != target_file || rank != target_rank; file += file_step, rank += rank_step) {
        if (!board[static_cast<std::size_t>(rank * 8 + file)].empty()) {
            return false;
        }
    }
    return true;
}

bool exchange_square_attacked_by(const std::array<Piece, 64>& board, int target, Color attacker) noexcept {
    for (int source = 0; source < 64; ++source) {
        if (board[static_cast<std::size_t>(source)].color == attacker &&
            exchange_piece_attacks_square(board, source, target)) {
            return true;
        }
    }
    return false;
}

bool exchange_recapture_is_legal(std::array<Piece, 64>& board, std::array<int, 2>& king_squares,
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

} // namespace

class GameState::Impl {
public:
    struct HistoryRecord {
        chess::Move move;
        bool null_move = false;
    };

    chess::Board board{};
    std::vector<HistoryRecord> history;
    struct FeatureCache {
        std::uint64_t position_key = 0;
        PositionFeatures features;
        bool valid = false;
    };
    mutable FeatureCache feature_cache;
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

int GameState::direct_static_exchange_gain(const MoveMetadata& initial) const noexcept {
    const Move move = initial.move;
    if (move.is_no_move() || move.from().index() == Square::kInvalid || move.to().index() == Square::kInvalid) {
        return 0;
    }
    if (!initial.is_capture() && initial.captured_piece == PieceType::none &&
        move.promotion() == Promotion::none) {
        return 0;
    }

    std::array<Piece, 64> board{};
    std::array<int, 2> king_squares{-1, -1};
    for (int square = 0; square < 64; ++square) {
        const chess::Piece native_piece = impl_->board.at(chess::Square(square));
        if (native_piece == chess::Piece::NONE) {
            continue;
        }
        const Piece piece{koi_piece_type(native_piece.type()), koi_color(native_piece.color())};
        board[static_cast<std::size_t>(square)] = piece;
        if (piece.type == PieceType::king) {
            king_squares[piece.color == Color::white ? 0 : 1] = square;
        }
    }

    const int source = move.from().index();
    const int target = move.to().index();
    const Piece moving_piece = board[static_cast<std::size_t>(source)];
    if (moving_piece.empty() || moving_piece.color != side_to_move() || moving_piece.type != initial.moving_piece ||
        king_squares[0] < 0 || king_squares[1] < 0) {
        return 0;
    }

    int captured_square = target;
    if (initial.kind == MoveKind::en_passant) {
        captured_square += moving_piece.color == Color::white ? -8 : 8;
    }
    if (captured_square < 0 || captured_square >= 64) {
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

        for (int square = 0; square < 64; ++square) {
            const Piece candidate = board[static_cast<std::size_t>(square)];
            if (candidate.empty() || candidate.color != side ||
                !exchange_piece_attacks_square(board, square, target)) {
                continue;
            }

            Piece replacement = candidate;
            int candidate_promotion_gain = 0;
            if (candidate.type == PieceType::pawn && (target / 8 == 0 || target / 8 == 7)) {
                replacement.type = PieceType::queen;
                candidate_promotion_gain = exchange_piece_value(PieceType::queen) - exchange_piece_value(PieceType::pawn);
            }
            if (!exchange_recapture_is_legal(board, king_squares, side, square, target, replacement)) {
                continue;
            }

            const int value = exchange_piece_value(candidate.type);
            if (value < attacker_value) {
                attacker_square = square;
                attacker_value = value;
                recapturing_piece = replacement;
                recapture_promotion_gain = candidate_promotion_gain;
            }
        }

        if (attacker_square < 0 || depth + 1 >= kMaximumExchangeDepth) {
            break;
        }

        const Piece target_piece = board[static_cast<std::size_t>(target)];
        ++depth;
        gains[depth] = exchange_piece_value(target_piece.type) - gains[depth - 1] + recapture_promotion_gain;

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

std::vector<Move> GameState::legal_moves() const {
    chess::Movelist native_moves;
    chess::movegen::legalmoves(native_moves, impl_->board);

    std::vector<Move> moves;
    moves.reserve(static_cast<std::size_t>(native_moves.size()));
    // The native generator is already legal for normal game positions. If an
    // externally supplied FEN has the opponent already in check, it can also
    // expose a king-capture pseudo-move; sanitize only that invalid-position
    // boundary instead of paying isLegal() for every move in every node.
    const chess::Color side_to_move = impl_->board.sideToMove();
    const bool sanitize_opponent_check =
        chess::attacks::attackers(impl_->board, side_to_move,
                                  impl_->board.kingSq(~side_to_move)).count() != 0;
    for (chess::Move native_move : native_moves) {
        if (!sanitize_opponent_check || impl_->board.isLegal(native_move)) {
            moves.push_back(koi_move_for(native_move));
        }
    }
    return moves;
}

std::vector<MoveMetadata> GameState::legal_moves_with_metadata() const {
    MoveMetadataList fixed_moves;
    legal_moves_with_metadata(fixed_moves);

    std::vector<MoveMetadata> moves;
    moves.reserve(fixed_moves.size());
    for (const MoveMetadata& metadata : fixed_moves) {
        moves.push_back(metadata);
    }
    return moves;
}

void GameState::legal_moves_with_metadata(MoveMetadataList& moves,
                                          bool include_check_flags) const noexcept {
    moves.clear();
    chess::Movelist native_moves;
    chess::movegen::legalmoves(native_moves, impl_->board);

    const chess::Color side_to_move = impl_->board.sideToMove();
    const bool sanitize_opponent_check =
        chess::attacks::attackers(impl_->board, side_to_move,
                                  impl_->board.kingSq(~side_to_move)).count() != 0;
    for (chess::Move native_move : native_moves) {
        if (sanitize_opponent_check && !impl_->board.isLegal(native_move)) {
            continue;
        }
        const Move move = koi_move_for(native_move);
        const bool gives_check = include_check_flags &&
            impl_->board.givesCheck(native_move) != chess::CheckType::NO_CHECK;
        if (!moves.push_back(metadata_for_native_move(impl_->board, native_move, move, gives_check))) {
            break;
        }
    }
}

bool GameState::legal_tactical_moves_with_metadata(MoveMetadataList& moves,
                                                   bool include_quiet_checks) const noexcept {
    moves.clear();

    const chess::Color side_to_move = impl_->board.sideToMove();
    const bool sanitize_opponent_check =
        chess::attacks::attackers(impl_->board, side_to_move,
                                  impl_->board.kingSq(~side_to_move)).count() != 0;
    const bool checked = impl_->board.inCheck();
    bool has_legal_move = false;

    const auto append_if_legal = [&](chess::Move native_move, bool tactical_only,
                                     bool verify_legality = false) noexcept {
        if ((sanitize_opponent_check || verify_legality) && !impl_->board.isLegal(native_move)) {
            return;
        }

        has_legal_move = true;
        if (tactical_only) {
            const bool gives_check = impl_->board.givesCheck(native_move) != chess::CheckType::NO_CHECK;
            if (native_move.typeOf() != chess::Move::PROMOTION &&
                !impl_->board.isCapture(native_move) && !gives_check) {
                return;
            }
            const Move move = koi_move_for(native_move);
            (void)moves.push_back(metadata_for_native_move(impl_->board, native_move, move, gives_check));
            return;
        }

        const Move move = koi_move_for(native_move);
        const bool gives_check = impl_->board.givesCheck(native_move) != chess::CheckType::NO_CHECK;
        (void)moves.push_back(metadata_for_native_move(impl_->board, native_move, move, gives_check));
    };

    if (checked) {
        chess::Movelist evasions;
        chess::movegen::legalmoves(evasions, impl_->board);
        for (chess::Move native_move : evasions) {
            append_if_legal(native_move, false);
        }
        return has_legal_move;
    }

    has_legal_move = sanitize_opponent_check ? [&] {
        chess::Movelist legal_moves;
        chess::movegen::legalmoves(legal_moves, impl_->board);
        return std::any_of(legal_moves.begin(), legal_moves.end(), [this](chess::Move native_move) {
            return impl_->board.isLegal(native_move);
        });
    }() : chess::movegen::anylegalmoves(impl_->board);

    chess::Movelist captures;
    chess::movegen::legalmoves<chess::movegen::MoveGenType::CAPTURE>(captures, impl_->board);
    for (chess::Move native_move : captures) {
        append_if_legal(native_move, true);
    }

    if (include_quiet_checks) {
        chess::Movelist quiet_moves;
        chess::movegen::legalmoves<chess::movegen::MoveGenType::QUIET>(quiet_moves, impl_->board);
        for (chess::Move native_move : quiet_moves) {
            append_if_legal(native_move, true);
        }
    } else {
        const int source_rank = side_to_move == chess::Color::WHITE ? 6 : 1;
        const int target_rank = side_to_move == chess::Color::WHITE ? 7 : 0;
        for (int file = 0; file < 8; ++file) {
            const chess::Square source(source_rank * 8 + file);
            const chess::Square target(target_rank * 8 + file);
            if (impl_->board.at(source).type() != chess::PieceType::PAWN ||
                impl_->board.at(source).color() != side_to_move ||
                impl_->board.at(target) != chess::Piece::NONE) {
                continue;
            }
            for (const chess::PieceType promotion : {chess::PieceType::QUEEN, chess::PieceType::ROOK,
                                                      chess::PieceType::BISHOP, chess::PieceType::KNIGHT}) {
                append_if_legal(chess::Move::make<chess::Move::PROMOTION>(source, target, promotion), true,
                                true);
            }
        }
    }

    return has_legal_move;
}

std::optional<MoveMetadata> GameState::describe_move(const Move& move) const noexcept {
    if (move.is_no_move()) {
        return std::nullopt;
    }

    const chess::Move native_move = native_move_for(impl_->board, move);
    if (native_move.move() == chess::Move::NO_MOVE || !impl_->board.isLegal(native_move)) {
        return std::nullopt;
    }
    const bool gives_check = impl_->board.givesCheck(native_move) != chess::CheckType::NO_CHECK;
    return metadata_for_native_move(impl_->board, native_move, move, gives_check);
}

PositionFeatures GameState::position_features() const noexcept {
    const std::uint64_t key = position_key();
    if (impl_->feature_cache.valid && impl_->feature_cache.position_key == key) {
        return impl_->feature_cache.features;
    }

    PositionFeatures features;
    features.side_to_move = side_to_move();
    std::array<std::uint64_t, 2> attacks{};
    const chess::Bitboard occupied = impl_->board.occ();
    for (std::uint8_t index = 0; index < 64; ++index) {
        const chess::Piece piece = impl_->board.at(chess::Square(index));
        if (piece == chess::Piece::NONE) {
            continue;
        }

        features.board[index] = {koi_piece_type(piece.type()), koi_color(piece.color())};
        const chess::Square square(index);
        const std::size_t color_index = piece.color() == chess::Color::WHITE ? 0 : 1;
        if (piece.type() == chess::PieceType::PAWN) {
            features.pawn_file_masks[color_index] = static_cast<std::uint8_t>(
                features.pawn_file_masks[color_index] | (std::uint8_t{1} << square.file()));
        }
        switch (static_cast<int>(piece.type())) {
        case static_cast<int>(chess::PieceType::KNIGHT):
        case static_cast<int>(chess::PieceType::BISHOP):
            features.game_phase = static_cast<std::uint8_t>(features.game_phase + 1);
            break;
        case static_cast<int>(chess::PieceType::ROOK):
            features.game_phase = static_cast<std::uint8_t>(features.game_phase + 2);
            break;
        case static_cast<int>(chess::PieceType::QUEEN):
            features.game_phase = static_cast<std::uint8_t>(features.game_phase + 4);
            break;
        default:
            break;
        }

        chess::Bitboard piece_attacks;
        switch (static_cast<int>(piece.type())) {
        case static_cast<int>(chess::PieceType::PAWN):
            piece_attacks = chess::attacks::pawn(piece.color(), square);
            break;
        case static_cast<int>(chess::PieceType::KNIGHT):
            piece_attacks = chess::attacks::knight(square);
            break;
        case static_cast<int>(chess::PieceType::BISHOP):
            piece_attacks = chess::attacks::bishop(square, occupied);
            break;
        case static_cast<int>(chess::PieceType::ROOK):
            piece_attacks = chess::attacks::rook(square, occupied);
            break;
        case static_cast<int>(chess::PieceType::QUEEN):
            piece_attacks = chess::attacks::queen(square, occupied);
            break;
        case static_cast<int>(chess::PieceType::KING):
            piece_attacks = chess::attacks::king(square);
            break;
        case static_cast<int>(chess::PieceType::NONE):
            continue;
        }
        attacks[color_index] |= piece_attacks.getBits();
    }

    features.game_phase = std::min<std::uint8_t>(features.game_phase, 24);

    const chess::Color colors[] = {chess::Color::WHITE, chess::Color::BLACK};
    for (std::size_t color_index = 0; color_index < 2; ++color_index) {
        const chess::Color color = colors[color_index];
        features.attacked_squares[color_index] = attacks[color_index];
        const std::uint64_t own = impl_->board.us(color).getBits();
        features.mobility[color_index] = static_cast<std::uint16_t>(
            std::popcount(attacks[color_index] & ~own));
        features.king_squares[color_index] = Square::from_index(
            static_cast<std::uint8_t>(impl_->board.kingSq(color).index()));
    }
    impl_->feature_cache = Impl::FeatureCache{key, features, true};
    return features;
}

bool GameState::is_legal(const Move& move) const noexcept {
    if (move.is_no_move()) {
        return false;
    }
    const chess::Move native_move = native_move_for(impl_->board, move);
    return native_move.move() != chess::Move::NO_MOVE && impl_->board.isLegal(native_move);
}

bool GameState::make_move(const Move& move) noexcept {
    const chess::Move native_move = native_move_for(impl_->board, move);
    if (native_move.move() == chess::Move::NO_MOVE) {
        return false;
    }
    if (!impl_->board.isLegal(native_move)) {
        return false;
    }
    try {
        impl_->history.push_back(Impl::HistoryRecord{native_move, false});
        try {
            impl_->board.makeMove(native_move);
        } catch (...) {
            impl_->history.pop_back();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool GameState::make_legal_move(const MoveMetadata& metadata) noexcept {
    const chess::Move native_move = native_move_for_metadata(metadata);
    if (native_move.move() == chess::Move::NO_MOVE) {
        return false;
    }
    try {
        impl_->history.push_back(Impl::HistoryRecord{native_move, false});
        try {
            impl_->board.makeMove(native_move);
        } catch (...) {
            impl_->history.pop_back();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool GameState::unmake_move() noexcept {
    if (impl_->history.empty()) {
        return false;
    }
    const Impl::HistoryRecord record = impl_->history.back();
    if (record.null_move) {
        return false;
    }
    impl_->board.unmakeMove(record.move);
    impl_->history.pop_back();
    return true;
}

bool GameState::make_null_move() noexcept {
    try {
        impl_->history.push_back(Impl::HistoryRecord{
            chess::Move{chess::Move::NULL_MOVE}, true});
        try {
            impl_->board.makeNullMove();
        } catch (...) {
            impl_->history.pop_back();
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool GameState::unmake_null_move() noexcept {
    if (impl_->history.empty() || !impl_->history.back().null_move) {
        return false;
    }
    impl_->board.unmakeNullMove();
    impl_->history.pop_back();
    return true;
}

bool GameState::is_capture(const Move& move) const noexcept {
    if (move.is_no_move()) {
        return false;
    }
    const chess::Move native_move = native_move_for(impl_->board, move);
    return native_move.move() != chess::Move::NO_MOVE && impl_->board.isLegal(native_move) &&
           impl_->board.isCapture(native_move);
}

bool GameState::in_check() const noexcept {
    return in_check(side_to_move());
}

bool GameState::in_check(Color color) const noexcept {
    const chess::Color native = native_color(color);
    return chess::attacks::attackers(impl_->board, ~native, impl_->board.kingSq(native)).count() != 0;
}

bool GameState::has_non_pawn_material(Color color) const noexcept {
    const chess::Color native = native_color(color);
    const std::uint64_t non_pawn = impl_->board.us(native).getBits() &
        ~(impl_->board.pieces(chess::PieceType::PAWN, native).getBits() |
          impl_->board.pieces(chess::PieceType::KING, native).getBits());
    return non_pawn != 0;
}

bool GameState::is_draw_by_rule() const noexcept {
    return impl_->board.isHalfMoveDraw() || impl_->board.isInsufficientMaterial() ||
        impl_->board.isRepetition();
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
