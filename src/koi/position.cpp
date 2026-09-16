#include "koi/position.hpp"

#include "koi/game_state.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace koi {

namespace {

constexpr std::uint8_t kWhiteKingSide = 0x1;
constexpr std::uint8_t kWhiteQueenSide = 0x2;
constexpr std::uint8_t kBlackKingSide = 0x4;
constexpr std::uint8_t kBlackQueenSide = 0x8;
constexpr std::uint8_t kAllCastling =
    kWhiteKingSide | kWhiteQueenSide | kBlackKingSide | kBlackQueenSide;
constexpr std::size_t kMaximumHistory = 256;

using Board = std::array<Piece, 64>;

class MoveBuffer {
public:
    template <typename... Arguments>
    void emplace_back(Arguments&&... arguments) noexcept {
        if (size_ >= storage_.size()) {
            return;
        }
        storage_[size_++] = Move(std::forward<Arguments>(arguments)...);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const Move* begin() const noexcept { return storage_.data(); }
    [[nodiscard]] const Move* end() const noexcept { return storage_.data() + size_; }

private:
    std::array<Move, kMaximumLegalMoves> storage_{};
    std::size_t size_ = 0;
};

struct ZobristKeys {
    std::array<std::array<std::array<std::uint64_t, 64>, 7>, 2> pieces{};
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 8> en_passant{};
    std::uint64_t black_to_move = 0;
};

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t extend_repetition_history_fingerprint(
    const std::uint64_t fingerprint, const std::uint64_t ancestor_key) noexcept {
    // This is an order-sensitive incremental combiner. The order is not
    // needed by repetition itself, but distinguishing different reversible
    // paths makes accidental TT context sharing less likely while snapshots
    // still make the operation exactly reversible on unmake.
    std::uint64_t seed = ancestor_key + 0x9E3779B97F4A7C15ULL;
    seed ^= fingerprint + 0xD6E8FEB86659FD93ULL +
        (seed << 6U) + (seed >> 2U);
    return splitmix64(seed) | 1ULL;
}

const ZobristKeys& zobrist() noexcept {
    static const ZobristKeys keys = [] {
        ZobristKeys result;
        std::uint64_t state = 0x4b4f492d76312e31ULL;
        for (std::size_t color = 0; color < 2; ++color) {
            for (std::size_t piece = 0; piece < 7; ++piece) {
                for (std::size_t square = 0; square < 64; ++square) {
                    result.pieces[color][piece][square] = splitmix64(state);
                }
            }
        }
        for (auto& value : result.castling) value = splitmix64(state);
        for (auto& value : result.en_passant) value = splitmix64(state);
        result.black_to_move = splitmix64(state);
        return result;
    }();
    return keys;
}

bool valid_square(int square) noexcept { return square >= 0 && square < 64; }
int file_of(int square) noexcept { return square & 7; }
int rank_of(int square) noexcept { return square >> 3; }

Piece piece_from_char(char value) noexcept {
    const Color color = std::isupper(static_cast<unsigned char>(value)) ?
        Color::white : Color::black;
    switch (static_cast<char>(std::tolower(static_cast<unsigned char>(value)))) {
    case 'p': return {PieceType::pawn, color};
    case 'n': return {PieceType::knight, color};
    case 'b': return {PieceType::bishop, color};
    case 'r': return {PieceType::rook, color};
    case 'q': return {PieceType::queen, color};
    case 'k': return {PieceType::king, color};
    default: return {};
    }
}

char piece_to_char(Piece piece) noexcept {
    char value = '?';
    switch (piece.type) {
    case PieceType::pawn: value = 'p'; break;
    case PieceType::knight: value = 'n'; break;
    case PieceType::bishop: value = 'b'; break;
    case PieceType::rook: value = 'r'; break;
    case PieceType::queen: value = 'q'; break;
    case PieceType::king: value = 'k'; break;
    case PieceType::none: return '1';
    }
    return piece.color == Color::white ? static_cast<char>(std::toupper(value)) : value;
}

bool parse_uint(std::string_view value, std::uint32_t& output, bool allow_zero,
                std::uint32_t maximum) noexcept {
    if (value.empty()) return false;
    std::uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() ||
        (!allow_zero && parsed == 0) || parsed > maximum) {
        return false;
    }
    output = parsed;
    return true;
}

bool split_fields(std::string_view fen, std::array<std::string_view, 6>& fields) noexcept {
    std::size_t count = 0;
    std::size_t cursor = 0;
    while (cursor < fen.size()) {
        while (cursor < fen.size() && std::isspace(static_cast<unsigned char>(fen[cursor]))) ++cursor;
        if (cursor == fen.size()) break;
        if (count == fields.size()) return false;
        const std::size_t begin = cursor;
        while (cursor < fen.size() && !std::isspace(static_cast<unsigned char>(fen[cursor]))) ++cursor;
        fields[count++] = fen.substr(begin, cursor - begin);
    }
    return count == fields.size();
}

struct Snapshot {
    Board board{};
    Color side = Color::white;
    std::uint8_t castling = 0;
    Square en_passant{};
    std::uint16_t halfmove = 0;
    std::uint16_t fullmove = 1;
    std::uint64_t key = 0;
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::array<std::uint64_t, 2> occupancy{};
    std::uint64_t occupied = 0;
    bool null_move = false;
    std::uint64_t repetition_history_fingerprint = 0;
    bool repetition_history_suppressed = false;
};

struct NativeState {
    Board board{};
    Color side = Color::white;
    std::uint8_t castling = kAllCastling;
    Square en_passant{};
    std::uint16_t halfmove = 0;
    std::uint16_t fullmove = 1;
    std::uint64_t key = 0;
    std::uint64_t repetition_history_fingerprint = 0;
    bool repetition_history_suppressed = false;
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::array<std::uint64_t, 2> occupancy{};
    std::uint64_t occupied = 0;
    std::array<Snapshot, kMaximumHistory> history{};
    std::size_t history_size = 0;
};

bool has_legal_en_passant_capture(const NativeState& state) noexcept;
bool has_legal_en_passant_capture_mutable(NativeState& state) noexcept;

std::uint64_t bit(int square) noexcept {
    return valid_square(square) ? (std::uint64_t{1} << square) : 0;
}

std::uint64_t calculate_key(const NativeState& state) noexcept {
    const ZobristKeys& keys = zobrist();
    std::uint64_t result = keys.castling[state.castling & kAllCastling];
    for (int square = 0; square < 64; ++square) {
        const Piece piece = state.board[static_cast<std::size_t>(square)];
        if (!piece.empty()) {
            result ^= keys.pieces[piece.color == Color::white ? 0 : 1]
                [static_cast<std::size_t>(piece.type)][static_cast<std::size_t>(square)];
        }
    }
    if (has_legal_en_passant_capture(state)) {
        result ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) result ^= keys.black_to_move;
    return result;
}

void rebuild_derived(NativeState& state) noexcept {
    state.piece_bitboards = {};
    state.occupancy = {};
    state.occupied = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = state.board[static_cast<std::size_t>(square)];
        if (piece.empty()) continue;
        const std::size_t color = piece.color == Color::white ? 0 : 1;
        state.piece_bitboards[color][static_cast<std::size_t>(piece.type)] |= bit(square);
        state.occupancy[color] |= bit(square);
    }
    state.occupied = state.occupancy[0] | state.occupancy[1];
    state.key = calculate_key(state);
}

void remove_derived_piece(NativeState& state, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    const std::uint64_t square_bit = bit(square);
    const std::size_t color = piece.color == Color::white ? 0 : 1;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] &= ~square_bit;
    state.occupancy[color] &= ~square_bit;
    state.key ^= zobrist().pieces[color][type][static_cast<std::size_t>(square)];
}

void add_derived_piece(NativeState& state, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    const std::uint64_t square_bit = bit(square);
    const std::size_t color = piece.color == Color::white ? 0 : 1;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] |= square_bit;
    state.occupancy[color] |= square_bit;
    state.key ^= zobrist().pieces[color][type][static_cast<std::size_t>(square)];
}

void remove_rule_keys(NativeState& state) noexcept {
    const ZobristKeys& keys = zobrist();
    state.key ^= keys.castling[state.castling & kAllCastling];
    if (has_legal_en_passant_capture_mutable(state)) {
        state.key ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) {
        state.key ^= keys.black_to_move;
    }
}

void add_rule_keys(NativeState& state) noexcept {
    const ZobristKeys& keys = zobrist();
    state.key ^= keys.castling[state.castling & kAllCastling];
    if (has_legal_en_passant_capture_mutable(state)) {
        state.key ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) {
        state.key ^= keys.black_to_move;
    }
}

int king_square(const NativeState& state, Color color) noexcept {
    for (int square = 0; square < 64; ++square) {
        const Piece piece = state.board[static_cast<std::size_t>(square)];
        if (piece.type == PieceType::king && piece.color == color) return square;
    }
    return -1;
}

bool aligned_slider_attacks(const Board& board, int source, int target, PieceType type) noexcept {
    const int file_delta = file_of(target) - file_of(source);
    const int rank_delta = rank_of(target) - rank_of(source);
    const int abs_file = std::abs(file_delta);
    const int abs_rank = std::abs(rank_delta);
    const bool diagonal = abs_file == abs_rank && abs_file != 0;
    const bool orthogonal = (file_delta == 0) != (rank_delta == 0);
    const bool valid = type == PieceType::bishop ? diagonal :
        type == PieceType::rook ? orthogonal : (diagonal || orthogonal);
    if (!valid) return false;
    const int file_step = file_delta == 0 ? 0 : (file_delta > 0 ? 1 : -1);
    const int rank_step = rank_delta == 0 ? 0 : (rank_delta > 0 ? 1 : -1);
    for (int file = file_of(source) + file_step, rank = rank_of(source) + rank_step;
         file != file_of(target) || rank != rank_of(target);
         file += file_step, rank += rank_step) {
        if (!board[static_cast<std::size_t>(rank * 8 + file)].empty()) return false;
    }
    return true;
}

bool square_attacked(const NativeState& state, int target, Color attacker) noexcept {
    if (!valid_square(target)) return false;
    const std::size_t color_index = attacker == Color::white ? 0U : 1U;
    const int target_file = file_of(target);
    const int target_rank = rank_of(target);

    // Pawn attackers are found by testing the two squares a pawn of the given
    // colour could attack from, instead of scanning the whole board.
    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    if (attacker == Color::white) {
        if (target_file >= 1 && (pawns & bit(target - 9)) != 0) return true;
        if (target_file <= 6 && (pawns & bit(target - 7)) != 0) return true;
    } else {
        if (target_file >= 1 && (pawns & bit(target + 7)) != 0) return true;
        if (target_file <= 6 && (pawns & bit(target + 9)) != 0) return true;
    }

    // Knights and kings move without blocking, so their attack test is a pure
    // geometric check over a handful of pieces.
    for (const PieceType type : {PieceType::knight, PieceType::king}) {
        std::uint64_t remaining =
            state.piece_bitboards[color_index][static_cast<std::size_t>(type)];
        while (remaining != 0) {
            const int source = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            const int abs_file = std::abs(target_file - file_of(source));
            const int abs_rank = std::abs(target_rank - rank_of(source));
            if (type == PieceType::knight) {
                if ((abs_file == 1 && abs_rank == 2) || (abs_file == 2 && abs_rank == 1)) {
                    return true;
                }
            } else if (abs_file <= 1 && abs_rank <= 1 && (abs_file != 0 || abs_rank != 0)) {
                return true;
            }
        }
    }

    // Sliders need the board for blocking, but only the pieces of the attacking
    // side have to be examined.
    for (const PieceType type : {PieceType::bishop, PieceType::rook, PieceType::queen}) {
        std::uint64_t remaining =
            state.piece_bitboards[color_index][static_cast<std::size_t>(type)];
        while (remaining != 0) {
            const int source = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            if (aligned_slider_attacks(state.board, source, target, type)) {
                return true;
            }
        }
    }
    return false;
}

int attacker_count(const NativeState& state, int target, Color attacker) noexcept {
    int count = 0;
    const std::size_t color_index = attacker == Color::white ? 0U : 1U;
    const int target_file = file_of(target);
    const int target_rank = rank_of(target);

    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    if (attacker == Color::white) {
        if (target_file >= 1 && (pawns & bit(target - 9)) != 0) ++count;
        if (target_file <= 6 && (pawns & bit(target - 7)) != 0) ++count;
    } else {
        if (target_file >= 1 && (pawns & bit(target + 7)) != 0) ++count;
        if (target_file <= 6 && (pawns & bit(target + 9)) != 0) ++count;
    }

    for (const PieceType type : {PieceType::knight, PieceType::king}) {
        std::uint64_t remaining =
            state.piece_bitboards[color_index][static_cast<std::size_t>(type)];
        while (remaining != 0) {
            const int source = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            const int abs_file = std::abs(target_file - file_of(source));
            const int abs_rank = std::abs(target_rank - rank_of(source));
            if (type == PieceType::knight) {
                if ((abs_file == 1 && abs_rank == 2) || (abs_file == 2 && abs_rank == 1)) {
                    ++count;
                }
            } else if (abs_file <= 1 && abs_rank <= 1 && (abs_file != 0 || abs_rank != 0)) {
                ++count;
            }
        }
    }

    for (const PieceType type : {PieceType::bishop, PieceType::rook, PieceType::queen}) {
        std::uint64_t remaining =
            state.piece_bitboards[color_index][static_cast<std::size_t>(type)];
        while (remaining != 0) {
            const int source = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;
            if (aligned_slider_attacks(state.board, source, target, type)) {
                ++count;
            }
        }
    }
    return count;
}

bool is_checked(const NativeState& state, Color color) noexcept {
    const int king = king_square(state, color);
    return king < 0 || square_attacked(state, king, opposite(color));
}

namespace {

// In-place variant used by the make/unmake hot path.  Applying the candidate
// capture directly to the board and the two pawn bitboards -- and restoring
// the exact same fields afterwards -- avoids the full NativeState copy
// (board + 256-entry history, roughly 75 KiB) that the read-only wrapper must
// still pay for its rare const callers.
bool has_legal_en_passant_capture_in_place(NativeState& state) noexcept {
    if (state.en_passant.index() >= Square::kInvalid) {
        return false;
    }

    const int target = state.en_passant.index();
    const Color side = state.side;
    const int source_rank = rank_of(target) - (side == Color::white ? 1 : -1);
    const int captured_square = target + (side == Color::white ? -8 : 8);
    if (!valid_square(captured_square) || source_rank < 0 || source_rank >= 8 ||
        !state.board[static_cast<std::size_t>(target)].empty()) {
        return false;
    }

    const Piece captured = state.board[static_cast<std::size_t>(captured_square)];
    if (captured.type != PieceType::pawn || captured.color != opposite(side)) {
        return false;
    }

    const std::size_t side_index = side == Color::white ? 0U : 1U;
    const std::size_t opposite_index = 1U - side_index;
    const std::size_t pawn_index = static_cast<std::size_t>(PieceType::pawn);
    for (const int source_file : {file_of(target) - 1, file_of(target) + 1}) {
        if (source_file < 0 || source_file >= 8) {
            continue;
        }
        const int source = source_rank * 8 + source_file;
        const Piece pawn = state.board[static_cast<std::size_t>(source)];
        if (pawn.type != PieceType::pawn || pawn.color != side) {
            continue;
        }

        const std::uint64_t source_bit = std::uint64_t{1} << source;
        const std::uint64_t captured_bit = std::uint64_t{1} << captured_square;
        const std::uint64_t target_bit = std::uint64_t{1} << target;
        state.board[static_cast<std::size_t>(source)] = {};
        state.board[static_cast<std::size_t>(captured_square)] = {};
        state.board[static_cast<std::size_t>(target)] = pawn;
        state.piece_bitboards[side_index][pawn_index] &= ~source_bit;
        state.piece_bitboards[side_index][pawn_index] |= target_bit;
        state.piece_bitboards[opposite_index][pawn_index] &= ~captured_bit;
        const bool legal = !is_checked(state, side);
        state.board[static_cast<std::size_t>(source)] = pawn;
        state.board[static_cast<std::size_t>(captured_square)] = captured;
        state.board[static_cast<std::size_t>(target)] = {};
        state.piece_bitboards[side_index][pawn_index] |= source_bit;
        state.piece_bitboards[side_index][pawn_index] &= ~target_bit;
        state.piece_bitboards[opposite_index][pawn_index] |= captured_bit;
        if (legal) {
            return true;
        }
    }
    return false;
}

} // namespace

bool has_legal_en_passant_capture_mutable(NativeState& state) noexcept {
    return has_legal_en_passant_capture_in_place(state);
}

bool has_legal_en_passant_capture(const NativeState& state) noexcept {
    NativeState scratch = state;
    return has_legal_en_passant_capture_in_place(scratch);
}

bool safe_king_step(NativeState& state, int from, int to, Color side) noexcept {
    const Piece source = state.board[static_cast<std::size_t>(from)];
    const Piece target = state.board[static_cast<std::size_t>(to)];
    state.board[static_cast<std::size_t>(from)] = {};
    state.board[static_cast<std::size_t>(to)] = source;
    const bool safe = !square_attacked(state, to, opposite(side));
    state.board[static_cast<std::size_t>(from)] = source;
    state.board[static_cast<std::size_t>(to)] = target;
    return safe;
}

template <typename MoveContainer>
void add_promotion_moves(MoveContainer& moves, Square from, Square to) {
    moves.emplace_back(from, to, Promotion::queen);
    moves.emplace_back(from, to, Promotion::rook);
    moves.emplace_back(from, to, Promotion::bishop);
    moves.emplace_back(from, to, Promotion::knight);
}

class NativePosition {
public:
    NativeState state{};

    NativePosition() { reset_startpos(); }

    bool set_fen(std::string_view fen, bool strict) noexcept {
        std::array<std::string_view, 6> fields{};
        if (!split_fields(fen, fields)) return false;

        NativeState candidate;
        candidate.board.fill({});
        int rank = 7;
        int file = 0;
        int white_kings = 0;
        int black_kings = 0;
        int white_pawns = 0;
        int black_pawns = 0;
        int white_pieces = 0;
        int black_pieces = 0;
        for (const char value : fields[0]) {
            if (value == '/') {
                if (file != 8 || rank == 0) return false;
                --rank;
                file = 0;
                continue;
            }
            if (value >= '1' && value <= '8') {
                file += value - '0';
                if (file > 8) return false;
                continue;
            }
            if (!std::string_view("PNBRQKpnbrqk").contains(value) || file >= 8 || rank < 0) {
                return false;
            }
            if ((value == 'P' || value == 'p') && (rank == 0 || rank == 7)) return false;
            const int square = rank * 8 + file++;
            candidate.board[static_cast<std::size_t>(square)] = piece_from_char(value);
            if (value == 'K') ++white_kings;
            if (value == 'k') ++black_kings;
            if (std::isupper(static_cast<unsigned char>(value))) {
                ++white_pieces;
                if (value == 'P') ++white_pawns;
            } else {
                ++black_pieces;
                if (value == 'p') ++black_pawns;
            }
        }
        if (rank != 0 || file != 8 || white_kings != 1 || black_kings != 1) {
            return false;
        }
        if (strict && (white_pawns > 8 || black_pawns > 8 ||
                       white_pieces > 16 || black_pieces > 16)) {
            return false;
        }
        if (fields[1] == "w") candidate.side = Color::white;
        else if (fields[1] == "b") candidate.side = Color::black;
        else return false;

        candidate.castling = 0;
        if (fields[2] != "-") {
            if (fields[2].empty() || fields[2].size() > 4) return false;
            for (std::size_t index = 0; index < fields[2].size(); ++index) {
                const char right = fields[2][index];
                if (!std::string_view("KQkq").contains(right) ||
                    fields[2].find(right, index + 1) != std::string_view::npos) return false;
                if (right == 'K') candidate.castling |= kWhiteKingSide;
                if (right == 'Q') candidate.castling |= kWhiteQueenSide;
                if (right == 'k') candidate.castling |= kBlackKingSide;
                if (right == 'q') candidate.castling |= kBlackQueenSide;
            }
        }
        if ((candidate.castling & kWhiteKingSide) != 0 &&
            (candidate.board[4].type != PieceType::king || candidate.board[4].color != Color::white ||
             candidate.board[7].type != PieceType::rook || candidate.board[7].color != Color::white)) return false;
        if ((candidate.castling & kWhiteQueenSide) != 0 &&
            (candidate.board[4].type != PieceType::king || candidate.board[4].color != Color::white ||
             candidate.board[0].type != PieceType::rook || candidate.board[0].color != Color::white)) return false;
        if ((candidate.castling & kBlackKingSide) != 0 &&
            (candidate.board[60].type != PieceType::king || candidate.board[60].color != Color::black ||
             candidate.board[63].type != PieceType::rook || candidate.board[63].color != Color::black)) return false;
        if ((candidate.castling & kBlackQueenSide) != 0 &&
            (candidate.board[60].type != PieceType::king || candidate.board[60].color != Color::black ||
             candidate.board[56].type != PieceType::rook || candidate.board[56].color != Color::black)) return false;

        std::uint32_t halfmove = 0;
        std::uint32_t fullmove = 0;
        if (!parse_uint(fields[4], halfmove, true, 255) || !parse_uint(fields[5], fullmove, false, 32768)) return false;
        candidate.halfmove = static_cast<std::uint16_t>(halfmove);
        candidate.fullmove = static_cast<std::uint16_t>(fullmove);
        if (fields[3] != "-") {
            if (candidate.halfmove != 0) return false;
            const auto parsed = Square::parse(fields[3]);
            if (!parsed.has_value()) return false;
            const int target = parsed->index();
            if ((candidate.side == Color::white && rank_of(target) != 5) ||
                (candidate.side == Color::black && rank_of(target) != 2) ||
                !candidate.board[static_cast<std::size_t>(target)].empty()) return false;
            const int behind = target + (candidate.side == Color::white ? -8 : 8);
            const int origin = target + (candidate.side == Color::white ? 8 : -8);
            const Piece expected{PieceType::pawn, opposite(candidate.side)};
            if (!valid_square(behind) || !valid_square(origin) ||
                candidate.board[static_cast<std::size_t>(behind)].type != expected.type ||
                candidate.board[static_cast<std::size_t>(behind)].color != expected.color ||
                !candidate.board[static_cast<std::size_t>(origin)].empty()) return false;
            candidate.en_passant = *parsed;
        }

        rebuild_derived(candidate);
        const int white_king = king_square(candidate, Color::white);
        const int black_king = king_square(candidate, Color::black);
        if (strict) {
            if (std::abs(file_of(white_king) - file_of(black_king)) <= 1 &&
                std::abs(rank_of(white_king) - rank_of(black_king)) <= 1) return false;
            if (attacker_count(candidate, white_king, Color::black) > 2 ||
                attacker_count(candidate, black_king, Color::white) > 2) return false;
            if (is_checked(candidate, Color::white) && is_checked(candidate, Color::black)) return false;
        }
        candidate.history_size = 0;
        state = candidate;
        return true;
    }

    void reset_startpos() noexcept {
        (void)set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", true);
    }

    std::string fen() const {
        std::string result;
        for (int rank = 7; rank >= 0; --rank) {
            int empty = 0;
            for (int file = 0; file < 8; ++file) {
                const Piece piece = state.board[static_cast<std::size_t>(rank * 8 + file)];
                if (piece.empty()) {
                    ++empty;
                } else {
                    if (empty != 0) result += static_cast<char>('0' + empty);
                    empty = 0;
                    result += piece_to_char(piece);
                }
            }
            if (empty != 0) result += static_cast<char>('0' + empty);
            if (rank != 0) result += '/';
        }
        result += state.side == Color::white ? " w " : " b ";
        if (state.castling == 0) result += '-';
        else {
            if (state.castling & kWhiteKingSide) result += 'K';
            if (state.castling & kWhiteQueenSide) result += 'Q';
            if (state.castling & kBlackKingSide) result += 'k';
            if (state.castling & kBlackQueenSide) result += 'q';
        }
        result += ' ';
        result += state.en_passant.index() < Square::kInvalid ? state.en_passant.uci() : "-";
        result += ' ' + std::to_string(state.halfmove);
        result += ' ' + std::to_string(state.fullmove);
        return result;
    }

    [[nodiscard]] Color side_to_move() const noexcept { return state.side; }

    [[nodiscard]] Piece piece_at(Square square) const noexcept {
        if (square.index() >= Square::kInvalid) {
            return {};
        }
        return state.board[square.index()];
    }

    std::vector<Move> legal_moves() {
        const Color mover = state.side;
        MoveBuffer pseudo;
        generate_pseudo(pseudo);
        std::vector<Move> result;
        result.reserve(pseudo.size());
        for (const Move& move : pseudo) {
            const Snapshot saved = snapshot();
            apply_unchecked(move);
            if (!is_checked(state, mover)) result.push_back(move);
            restore(saved);
        }
        return result;
    }

    std::size_t legal_moves_into(std::span<Move> output) {
        if (output.empty()) {
            return 0;
        }
        const Color mover = state.side;
        MoveBuffer pseudo;
        generate_pseudo(pseudo);
        std::size_t count = 0;
        for (const Move& move : pseudo) {
            const Snapshot saved = snapshot();
            apply_unchecked(move);
            if (!is_checked(state, mover) && count < output.size()) {
                output[count++] = move;
            }
            restore(saved);
        }
        return count;
    }

    [[nodiscard]] bool is_legal(const Move& move) const noexcept {
        if (move.is_no_move()) {
            return false;
        }
        try {
            NativePosition candidate = *this;
            const auto moves = candidate.legal_moves();
            return std::find(moves.begin(), moves.end(), move) != moves.end();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool is_capture(const Move& move) const noexcept {
        if (move.from().index() >= Square::kInvalid || move.to().index() >= Square::kInvalid) {
            return false;
        }
        const Piece moving = state.board[move.from().index()];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        const Piece target = state.board[move.to().index()];
        return (!target.empty() && target.color != moving.color) ||
            (moving.type == PieceType::pawn && target.empty() &&
             move.to() == state.en_passant);
    }

    bool apply_legal(const Move& move) {
        const auto legal = legal_moves();
        if (std::find(legal.begin(), legal.end(), move) == legal.end() ||
            state.history_size >= kMaximumHistory) return false;
        state.history[state.history_size++] = snapshot();
        apply_unchecked(move);
        return true;
    }

    bool make_move(const Move& move) noexcept {
        try {
            return apply_legal(move);
        } catch (...) {
            return false;
        }
    }

    bool make_generated_move(const Move& move) noexcept {
        if (state.history_size >= kMaximumHistory ||
            move.from().index() >= Square::kInvalid || move.to().index() >= Square::kInvalid) {
            return false;
        }
        const Piece moving = state.board[move.from().index()];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        state.history[state.history_size++] = snapshot();
        apply_unchecked(move);
        return true;
    }

    bool unapply() noexcept {
        if (state.history_size == 0) return false;
        const Snapshot saved = state.history[--state.history_size];
        restore(saved);
        return true;
    }

    bool unmake_move() noexcept {
        if (state.history_size == 0 || state.history[state.history_size - 1].null_move) {
            return false;
        }
        return unapply();
    }

    bool make_null_move() noexcept {
        if (state.history_size >= kMaximumHistory) {
            return false;
        }
        Snapshot saved = snapshot();
        saved.null_move = true;
        state.history[state.history_size++] = saved;
        state.en_passant = {};
        state.halfmove = static_cast<std::uint16_t>(
            std::min<unsigned>(std::numeric_limits<std::uint16_t>::max(), state.halfmove + 1));
        if (state.side == Color::black) {
            state.fullmove = static_cast<std::uint16_t>(
                std::min<unsigned>(std::numeric_limits<std::uint16_t>::max(), state.fullmove + 1));
        }
        state.side = opposite(state.side);
        // Null transitions are search-only passes. Keep the real reversible
        // history fingerprint intact, but suppress all positions derived from
        // the pass while that speculative branch remains active.
        state.repetition_history_suppressed = true;
        rebuild_derived(state);
        return true;
    }

    bool unmake_null_move() noexcept {
        if (state.history_size == 0 || !state.history[state.history_size - 1].null_move) {
            return false;
        }
        return unapply();
    }

    [[nodiscard]] std::uint64_t position_key() const noexcept { return state.key; }

    [[nodiscard]] std::uint64_t repetition_history_fingerprint() const noexcept {
        return state.repetition_history_fingerprint;
    }

    [[nodiscard]] bool repetition_history_suppressed() const noexcept {
        return state.repetition_history_suppressed;
    }

    [[nodiscard]] std::uint64_t pawn_key() const noexcept {
        constexpr std::size_t pawn = static_cast<std::size_t>(PieceType::pawn);
        // The native state maintains these bitboards incrementally. Mix the
        // two colors before the history table masks the result so nearby
        // board squares do not create a biased bucket distribution.
        std::uint64_t seed = state.piece_bitboards[0][pawn] ^
            state.piece_bitboards[1][pawn] * 0xD6E8FEB86659FD93ULL;
        return splitmix64(seed);
    }

    [[nodiscard]] std::size_t piece_count() const noexcept {
        std::size_t count = 0;
        for (const Piece& piece : state.board) {
            if (!piece.empty()) ++count;
        }
        return count;
    }

    [[nodiscard]] std::uint64_t piece_bitboard(PieceType type, Color color) const noexcept {
        const std::size_t type_index = static_cast<std::size_t>(type);
        if (type_index >= state.piece_bitboards[0].size()) {
            return 0;
        }
        return state.piece_bitboards[color == Color::white ? 0 : 1][type_index];
    }

    [[nodiscard]] bool in_check() const noexcept { return is_checked(state, state.side); }

    [[nodiscard]] bool in_check(Color color) const noexcept { return is_checked(state, color); }

    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept {
        for (const Piece& piece : state.board) {
            if (piece.color == color && !piece.empty() &&
                piece.type != PieceType::pawn && piece.type != PieceType::king) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint8_t castling_rights() const noexcept { return state.castling; }

    [[nodiscard]] Square en_passant_square() const noexcept { return state.en_passant; }

    [[nodiscard]] std::size_t history_size() const noexcept { return state.history_size; }

    [[nodiscard]] std::size_t repetition_count() const noexcept {
        // A null move is a search-only pass, not a legal game-history entry.
        // Its descendants may still make ordinary moves before the probe is
        // undone, so checking only for the most recent null snapshot would
        // let those artificial positions participate in repetition draws.
        if (state.repetition_history_suppressed) {
            return 1;
        }
        std::size_t count = 1;
        for (std::size_t index = state.history_size; index > 0; --index) {
            const Snapshot& previous = state.history[index - 1];
            if (previous.null_move) {
                break;
            }
            if (previous.key == state.key) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] bool is_checkmate() const noexcept {
        try {
            auto* mutable_this = const_cast<NativePosition*>(this);
            return mutable_this->legal_moves().empty() && in_check();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool can_claim_threefold_repetition() const noexcept {
        return repetition_count() >= 3 && !is_checkmate();
    }

    [[nodiscard]] bool can_claim_fifty_move_draw() const noexcept {
        // A search null move advances the stored compatibility clock, but it
        // is not a legal game move and must not create a claimable draw by
        // itself (or while a reversible descendant is still on that branch).
        return !state.repetition_history_suppressed && state.halfmove >= 100 &&
            !is_checkmate();
    }

    [[nodiscard]] bool is_automatic_fivefold_repetition() const noexcept {
        return repetition_count() >= 5 && !is_checkmate();
    }

    [[nodiscard]] bool is_automatic_seventy_five_move_draw() const noexcept {
        return !state.repetition_history_suppressed && state.halfmove >= 150 &&
            !is_checkmate();
    }

    [[nodiscard]] bool is_insufficient_material() const noexcept {
        int non_king = 0;
        int bishops = 0;
        int knights = 0;
        int bishop_color = -1;
        for (int square = 0; square < 64; ++square) {
            const Piece piece = state.board[static_cast<std::size_t>(square)];
            if (piece.empty() || piece.type == PieceType::king) {
                continue;
            }
            if (piece.type == PieceType::pawn || piece.type == PieceType::rook ||
                piece.type == PieceType::queen) {
                return false;
            }
            ++non_king;
            if (piece.type == PieceType::knight) {
                ++knights;
            } else if (piece.type == PieceType::bishop) {
                ++bishops;
                const int square_color = (file_of(square) + rank_of(square)) & 1;
                if (bishop_color < 0) {
                    bishop_color = square_color;
                } else if (bishop_color != square_color) {
                    bishop_color = 2;
                }
            }
        }
        if (non_king <= 1) {
            return true;
        }
        return knights == 0 && bishops == non_king && bishop_color >= 0 && bishop_color < 2;
    }

    [[nodiscard]] bool is_known_locked_pawn_wall() const noexcept {
        // Keep dead-position recognition intentionally narrow. This blocked
        // pawn wall is a known FIDE dead-position example; the board is what
        // makes it dead, not the side to move or the bookkeeping fields in a
        // FEN. An actually legal en-passant capture is an exception because it
        // creates a pawn continuation from an otherwise locked wall. Compare
        // the normalized board directly so a normal clock/full-move update
        // cannot make the same dead position searchable again.
        if (has_legal_en_passant_capture(state)) {
            return false;
        }
        static constexpr Board pattern = [] {
            Board result{};
            result[17] = {PieceType::pawn, Color::white};
            result[19] = {PieceType::bishop, Color::white};
            result[20] = {PieceType::king, Color::white};
            result[24] = {PieceType::pawn, Color::white};
            result[25] = {PieceType::pawn, Color::black};
            result[26] = {PieceType::pawn, Color::white};
            result[28] = {PieceType::pawn, Color::white};
            result[32] = {PieceType::pawn, Color::black};
            result[34] = {PieceType::pawn, Color::black};
            result[36] = {PieceType::pawn, Color::black};
            result[39] = {PieceType::pawn, Color::white};
            result[50] = {PieceType::bishop, Color::black};
            result[52] = {PieceType::king, Color::black};
            result[47] = {PieceType::pawn, Color::black};
            return result;
        }();
        for (std::size_t square = 0; square < pattern.size(); ++square) {
            const Piece actual = state.board[square];
            const Piece expected = pattern[square];
            if (actual.type != expected.type ||
                (!actual.empty() && actual.color != expected.color)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool is_dead_position() const noexcept {
        return is_insufficient_material() || is_known_locked_pawn_wall();
    }

    [[nodiscard]] DrawStatus draw_status() const noexcept {
        if (is_dead_position()) return DrawStatus::dead_position;
        if (is_automatic_seventy_five_move_draw()) {
            return DrawStatus::automatic_seventy_five_move;
        }
        if (is_automatic_fivefold_repetition()) return DrawStatus::automatic_fivefold;
        if (can_claim_fifty_move_draw()) return DrawStatus::claimable_fifty_move;
        if (can_claim_threefold_repetition()) return DrawStatus::claimable_threefold;
        return DrawStatus::none;
    }

    [[nodiscard]] bool is_repetition_sensitive() const noexcept {
        return repetition_count() >= 2;
    }

    [[nodiscard]] bool is_draw_by_rule() const noexcept {
        return draw_status() != DrawStatus::none;
    }

    [[nodiscard]] bool is_terminal() noexcept {
        const DrawStatus status = draw_status();
        if (status == DrawStatus::dead_position ||
            status == DrawStatus::automatic_fivefold ||
            status == DrawStatus::automatic_seventy_five_move) {
            return true;
        }
        try {
            // Checkmate takes precedence over claimable or automatic draw
            // clocks; otherwise a no-move position is terminal as stalemate.
            return legal_moves().empty();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept { return state.halfmove; }
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept { return state.fullmove; }

private:
    Snapshot snapshot() const noexcept {
        return Snapshot{state.board, state.side, state.castling, state.en_passant,
                         state.halfmove, state.fullmove, state.key, state.piece_bitboards,
                         state.occupancy, state.occupied, false,
                         state.repetition_history_fingerprint,
                         state.repetition_history_suppressed};
    }

    void restore(const Snapshot& saved) noexcept {
        state.board = saved.board;
        state.side = saved.side;
        state.castling = saved.castling;
        state.en_passant = saved.en_passant;
        state.halfmove = saved.halfmove;
        state.fullmove = saved.fullmove;
        state.key = saved.key;
        state.repetition_history_fingerprint = saved.repetition_history_fingerprint;
        state.repetition_history_suppressed = saved.repetition_history_suppressed;
        state.piece_bitboards = saved.piece_bitboards;
        state.occupancy = saved.occupancy;
        state.occupied = saved.occupied;
    }

    template <typename MoveContainer>
    void push(MoveContainer& moves, int from, int to) const {
        if (!valid_square(to)) return;
        const Piece target = state.board[static_cast<std::size_t>(to)];
        if (!target.empty() && target.color == state.side) return;
        if (target.type == PieceType::king) return;
        const Square source = Square::from_index(static_cast<std::uint8_t>(from));
        const Square destination = Square::from_index(static_cast<std::uint8_t>(to));
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const int promotion_rank = moving.color == Color::white ? 7 : 0;
        if (moving.type == PieceType::pawn && rank_of(to) == promotion_rank) {
            add_promotion_moves(moves, source, destination);
        } else {
            moves.emplace_back(source, destination);
        }
    }

    template <typename MoveContainer>
    void generate_pseudo(MoveContainer& moves) {
        const Color side = state.side;
        for (int from = 0; from < 64; ++from) {
            const Piece piece = state.board[static_cast<std::size_t>(from)];
            if (piece.empty() || piece.color != side) continue;
            const int file = file_of(from);
            const int rank = rank_of(from);
            if (piece.type == PieceType::pawn) {
                const int direction = side == Color::white ? 1 : -1;
                const int one = from + direction * 8;
                if (valid_square(one) && state.board[static_cast<std::size_t>(one)].empty()) {
                    push(moves, from, one);
                    const int two = from + direction * 16;
                    const int start_rank = side == Color::white ? 1 : 6;
                    if (rank == start_rank && state.board[static_cast<std::size_t>(two)].empty()) {
                        moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                           Square::from_index(static_cast<std::uint8_t>(two)));
                    }
                }
                for (const int file_delta : {-1, 1}) {
                    const int target_file = file + file_delta;
                    const int target_rank = rank + direction;
                    if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) continue;
                    const int to = target_rank * 8 + target_file;
                    const Piece target = state.board[static_cast<std::size_t>(to)];
                    if ((!target.empty() && target.color != side && target.type != PieceType::king) ||
                        (state.en_passant.index() == to && target.empty())) {
                        push(moves, from, to);
                    }
                }
                continue;
            }
            if (piece.type == PieceType::knight || piece.type == PieceType::king) {
                static constexpr std::array<std::array<int, 2>, 8> knight_steps{{
                    {{1, 2}}, {{2, 1}}, {{2, -1}}, {{1, -2}},
                    {{-1, -2}}, {{-2, -1}}, {{-2, 1}}, {{-1, 2}}}};
                static constexpr std::array<std::array<int, 2>, 8> king_steps{{
                    {{1, 1}}, {{1, 0}}, {{1, -1}}, {{0, -1}},
                    {{-1, -1}}, {{-1, 0}}, {{-1, 1}}, {{0, 1}}}};
                const auto& steps = piece.type == PieceType::knight ? knight_steps : king_steps;
                for (const auto& step : steps) {
                    const int target_file = file + step[0];
                    const int target_rank = rank + step[1];
                    if (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                        push(moves, from, target_rank * 8 + target_file);
                    }
                }
                if (piece.type == PieceType::king && !is_checked(state, side)) {
                    const int rank_base = side == Color::white ? 0 : 7;
                    if (from == rank_base * 8 + 4) {
                        const std::uint8_t king_right = side == Color::white ? kWhiteKingSide : kBlackKingSide;
                        const std::uint8_t queen_right = side == Color::white ? kWhiteQueenSide : kBlackQueenSide;
                        if ((state.castling & king_right) != 0 &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 5)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 6)].empty() &&
                            safe_king_step(state, from, rank_base * 8 + 5, side) &&
                            safe_king_step(state, from, rank_base * 8 + 6, side)) {
                            moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                               Square::from_index(static_cast<std::uint8_t>(rank_base * 8 + 6)));
                        }
                        if ((state.castling & queen_right) != 0 &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 1)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 2)].empty() &&
                            state.board[static_cast<std::size_t>(rank_base * 8 + 3)].empty() &&
                            safe_king_step(state, from, rank_base * 8 + 3, side) &&
                            safe_king_step(state, from, rank_base * 8 + 2, side)) {
                            moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                               Square::from_index(static_cast<std::uint8_t>(rank_base * 8 + 2)));
                        }
                    }
                }
                continue;
            }

            const bool diagonal = piece.type == PieceType::bishop || piece.type == PieceType::queen;
            const bool orthogonal = piece.type == PieceType::rook || piece.type == PieceType::queen;
            static constexpr std::array<std::array<int, 2>, 4> diagonal_steps{{
                {{1, 1}}, {{1, -1}}, {{-1, -1}}, {{-1, 1}}}};
            static constexpr std::array<std::array<int, 2>, 4> orthogonal_steps{{
                {{1, 0}}, {{0, 1}}, {{-1, 0}}, {{0, -1}}}};
            const auto add_rays = [&moves, this, from, file, rank](const auto& steps) {
                for (const auto& step : steps) {
                    int target_file = file + step[0];
                    int target_rank = rank + step[1];
                    while (target_file >= 0 && target_file < 8 && target_rank >= 0 && target_rank < 8) {
                        const int to = target_rank * 8 + target_file;
                        const Piece target = state.board[static_cast<std::size_t>(to)];
                        if (target.empty()) {
                            moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                               Square::from_index(static_cast<std::uint8_t>(to)));
                        } else {
                            if (target.color != state.side && target.type != PieceType::king) {
                                moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                                   Square::from_index(static_cast<std::uint8_t>(to)));
                            }
                            break;
                        }
                        target_file += step[0];
                        target_rank += step[1];
                    }
                }
            };
            if (diagonal) add_rays(diagonal_steps);
            if (orthogonal) add_rays(orthogonal_steps);
        }
    }

    void apply_unchecked(const Move& move) noexcept {
        const int from = move.from().index();
        const int to = move.to().index();
        const std::uint64_t previous_position_key = state.key;
        const std::uint8_t previous_castling = state.castling;
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const Piece captured = state.board[static_cast<std::size_t>(to)];
        const bool pawn_move = moving.type == PieceType::pawn;
        const bool castling = moving.type == PieceType::king && std::abs(file_of(to) - file_of(from)) == 2;
        const bool en_passant = pawn_move && to == state.en_passant.index() && captured.empty();
        const bool capture = !captured.empty() || en_passant;

        // Remove the old rule-state keys and every piece that is about to
        // leave the board before mutating the board array. The remaining
        // derived state is rebuilt only from the pieces that actually moved.
        remove_rule_keys(state);
        remove_derived_piece(state, moving, from);
        if (!captured.empty()) {
            remove_derived_piece(state, captured, to);
        }
        const int en_passant_captured_square =
            en_passant ? to + (moving.color == Color::white ? -8 : 8) : -1;
        if (en_passant) {
            remove_derived_piece(
                state, state.board[static_cast<std::size_t>(en_passant_captured_square)],
                en_passant_captured_square);
        }

        state.en_passant = {};
        state.board[static_cast<std::size_t>(from)] = {};
        if (en_passant) {
            state.board[static_cast<std::size_t>(en_passant_captured_square)] = {};
        }
        state.board[static_cast<std::size_t>(to)] = moving;
        if (move.promotion() != Promotion::none) {
            PieceType promoted = PieceType::queen;
            if (move.promotion() == Promotion::rook) promoted = PieceType::rook;
            if (move.promotion() == Promotion::bishop) promoted = PieceType::bishop;
            if (move.promotion() == Promotion::knight) promoted = PieceType::knight;
            state.board[static_cast<std::size_t>(to)].type = promoted;
        }
        add_derived_piece(state, state.board[static_cast<std::size_t>(to)], to);
        if (castling) {
            const int rook_from = to > from ? from + 3 : from - 4;
            const int rook_to = to > from ? from + 1 : from - 1;
            const Piece rook = state.board[static_cast<std::size_t>(rook_from)];
            remove_derived_piece(state, rook, rook_from);
            state.board[static_cast<std::size_t>(rook_to)] = rook;
            state.board[static_cast<std::size_t>(rook_from)] = {};
            add_derived_piece(state, rook, rook_to);
        }

        if (moving.type == PieceType::king) {
            state.castling &= moving.color == Color::white ?
                static_cast<std::uint8_t>(~(kWhiteKingSide | kWhiteQueenSide)) :
                static_cast<std::uint8_t>(~(kBlackKingSide | kBlackQueenSide));
        }
        const auto clear_rook_right = [this](int square) {
            if (square == 0) state.castling &= static_cast<std::uint8_t>(~kWhiteQueenSide);
            if (square == 7) state.castling &= static_cast<std::uint8_t>(~kWhiteKingSide);
            if (square == 56) state.castling &= static_cast<std::uint8_t>(~kBlackQueenSide);
            if (square == 63) state.castling &= static_cast<std::uint8_t>(~kBlackKingSide);
        };
        if (moving.type == PieceType::rook) clear_rook_right(from);
        if (captured.type == PieceType::rook) clear_rook_right(to);
        if (pawn_move && std::abs(to - from) == 16) {
            state.en_passant = Square::from_index(static_cast<std::uint8_t>((to + from) / 2));
        }
        state.halfmove = pawn_move || capture ? 0 :
            static_cast<std::uint16_t>(std::min<unsigned>(255, state.halfmove + 1));
        if (state.side == Color::black) {
            state.fullmove = static_cast<std::uint16_t>(std::min<unsigned>(32768, state.fullmove + 1));
        }
        state.side = opposite(state.side);
        if (state.en_passant.index() < Square::kInvalid &&
            !has_legal_en_passant_capture_mutable(state)) {
            state.en_passant = {};
        }
        state.occupied = state.occupancy[0] | state.occupancy[1];
        add_rule_keys(state);
        const bool irreversible = pawn_move || capture || state.castling != previous_castling;
        if (!state.repetition_history_suppressed) {
            state.repetition_history_fingerprint = irreversible ? 0 :
                extend_repetition_history_fingerprint(
                    state.repetition_history_fingerprint, previous_position_key);
        } else if (irreversible) {
            // Do not let a real move made after a search null move turn the
            // artificial null branch into a legal repetition ancestor. This
            // irreversible move starts a fresh real-history segment, so later
            // reversible moves must be tracked again.
            state.repetition_history_fingerprint = 0;
            state.repetition_history_suppressed = false;
        }
    }
};

} // namespace

class Position::Impl {
public:
    NativePosition position;
};

Position::Position() : impl_(std::make_unique<Impl>()) {}

Position::Position(std::string_view fen) : impl_(std::make_unique<Impl>()) {
    if (!impl_->position.set_fen(fen, true)) throw std::invalid_argument("invalid FEN");
}

Position::Position(const Position& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Position::Position(Position&&) noexcept = default;

Position& Position::operator=(const Position& other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}

Position& Position::operator=(Position&&) noexcept = default;
Position::~Position() = default;

std::string Position::fen() const { return impl_->position.fen(); }

Color Position::side_to_move() const noexcept { return impl_->position.side_to_move(); }

Piece Position::piece_at(Square square) const noexcept { return impl_->position.piece_at(square); }

std::vector<Move> Position::legal_moves() const {
    return const_cast<NativePosition&>(impl_->position).legal_moves();
}

std::size_t Position::legal_moves_into(std::span<Move> output) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).legal_moves_into(output);
    } catch (...) {
        return 0;
    }
}

bool Position::is_legal(const Move& move) const noexcept { return impl_->position.is_legal(move); }

bool Position::is_capture(const Move& move) const noexcept { return impl_->position.is_capture(move); }

bool Position::make_move(const Move& move) noexcept { return impl_->position.make_move(move); }

bool Position::make_generated_move(const Move& move) noexcept {
    return impl_->position.make_generated_move(move);
}

bool Position::unmake_move() noexcept { return impl_->position.unmake_move(); }

bool Position::make_null_move() noexcept { return impl_->position.make_null_move(); }

bool Position::unmake_null_move() noexcept { return impl_->position.unmake_null_move(); }

bool Position::apply_uci(std::string_view uci) {
    const auto move = Move::parse_uci(uci);
    return move.has_value() && impl_->position.apply_legal(*move);
}

bool Position::unapply() { return impl_->position.unapply(); }

bool Position::set_fen(std::string_view fen) {
    NativePosition candidate = impl_->position;
    if (!candidate.set_fen(fen, true)) return false;
    impl_->position = std::move(candidate);
    return true;
}

bool Position::set_fen_unchecked(std::string_view fen) {
    NativePosition candidate = impl_->position;
    if (!candidate.set_fen(fen, false)) return false;
    impl_->position = std::move(candidate);
    return true;
}

std::uint64_t Position::position_key() const noexcept { return impl_->position.position_key(); }

std::uint64_t Position::repetition_history_fingerprint() const noexcept {
    return impl_->position.repetition_history_fingerprint();
}

bool Position::repetition_history_suppressed() const noexcept {
    return impl_->position.repetition_history_suppressed();
}

std::uint64_t Position::pawn_key() const noexcept { return impl_->position.pawn_key(); }

std::size_t Position::piece_count() const noexcept { return impl_->position.piece_count(); }

std::uint64_t Position::piece_bitboard(PieceType type, Color color) const noexcept {
    return impl_->position.piece_bitboard(type, color);
}

bool Position::in_check() const noexcept { return impl_->position.in_check(); }

bool Position::in_check(Color color) const noexcept { return impl_->position.in_check(color); }

bool Position::has_non_pawn_material(Color color) const noexcept {
    return impl_->position.has_non_pawn_material(color);
}

std::uint8_t Position::castling_rights() const noexcept {
    return impl_->position.castling_rights();
}

Square Position::en_passant_square() const noexcept {
    return impl_->position.en_passant_square();
}

std::size_t Position::repetition_count() const noexcept {
    return impl_->position.repetition_count();
}

bool Position::can_claim_threefold_repetition() const noexcept {
    return impl_->position.can_claim_threefold_repetition();
}

bool Position::can_claim_fifty_move_draw() const noexcept {
    return impl_->position.can_claim_fifty_move_draw();
}

bool Position::is_automatic_fivefold_repetition() const noexcept {
    return impl_->position.is_automatic_fivefold_repetition();
}

bool Position::is_automatic_seventy_five_move_draw() const noexcept {
    return impl_->position.is_automatic_seventy_five_move_draw();
}

bool Position::is_dead_position() const noexcept {
    return impl_->position.is_dead_position();
}

DrawStatus Position::draw_status() const noexcept {
    return impl_->position.draw_status();
}

bool Position::is_claimable_draw() const noexcept {
    const DrawStatus status = draw_status();
    return status == DrawStatus::claimable_threefold ||
        status == DrawStatus::claimable_fifty_move;
}

bool Position::is_forced_draw() const noexcept {
    const DrawStatus status = draw_status();
    return status == DrawStatus::dead_position ||
        status == DrawStatus::automatic_fivefold ||
        status == DrawStatus::automatic_seventy_five_move;
}

bool Position::is_repetition_sensitive() const noexcept {
    return impl_->position.is_repetition_sensitive();
}

bool Position::is_draw_by_rule() const noexcept { return impl_->position.is_draw_by_rule(); }

bool Position::is_terminal() const noexcept { return impl_->position.is_terminal(); }

std::uint16_t Position::halfmove_clock() const noexcept { return impl_->position.halfmove_clock(); }

std::uint16_t Position::fullmove_number() const noexcept { return impl_->position.fullmove_number(); }

std::size_t Position::history_size() const noexcept { return impl_->position.history_size(); }

} // namespace koi
