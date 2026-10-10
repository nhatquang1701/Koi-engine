#include "koi/position.hpp"

#include "koi/game_state.hpp"
#include "koi/detail/attack_tables.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
    // Uninitialized on purpose: every read is bounded by size_ and every write
    // goes through emplace_back, which stores the element before counting it.
    // Value-initializing would zero 256 Moves (1 KiB) on every generation call.
    std::array<Move, kMaximumLegalMoves> storage_;
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
    // Sits next to `key` on purpose: repetition_count() walks the history
    // backwards and reads exactly these two fields from every visited
    // snapshot, so one cache line now covers the whole probe.
    bool null_move = false;
    std::array<std::array<std::uint64_t, 7>, 2> piece_bitboards{};
    std::array<std::uint64_t, 2> occupancy{};
    std::uint64_t occupied = 0;
    std::array<std::uint64_t, 2> checkers{};
    std::array<std::uint64_t, 2> pinned{};
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
    std::array<std::uint64_t, 2> checkers{};
    std::array<std::uint64_t, 2> pinned{};
    std::array<Snapshot, kMaximumHistory> history{};
    std::size_t history_size = 0;
};

bool has_legal_en_passant_capture(const NativeState& state) noexcept;
bool has_legal_en_passant_capture_mutable(NativeState& state) noexcept;
void refresh_king_masks(NativeState& state) noexcept;

// Records a snapshot of the pre-move state in the next history slot. The
// array has a fixed capacity so the search can make and unmake moves without
// allocating, but a game may legitimately run past it: a long replay then
// drops the oldest snapshot and keeps the recent window that repetition
// detection and search need. Without this, a command listing more than
// `kMaximumHistory` moves would be rejected and the engine would keep
// answering the stale position.
//
// The slot is filled field by field straight from `state`, so make/unmake
// pays no intermediate Snapshot temporary. That is bit-identical to the
// former `record_snapshot(state, snapshot())`: the full-history memmove only
// shifts history[0..kMaximumHistory-2], which shares no storage with the
// copied state fields, so shifting before or after the copy writes the same
// values. Only struct padding is left as the slot had it, and nothing reads
// padding.
void snapshot_into(NativeState& state, bool null_move = false) noexcept {
    Snapshot* slot = nullptr;
    if (state.history_size >= kMaximumHistory) {
        std::memmove(state.history.data(), state.history.data() + 1,
                     (kMaximumHistory - 1) * sizeof(Snapshot));
        slot = &state.history[kMaximumHistory - 1];
    } else {
        slot = &state.history[state.history_size++];
    }
    slot->board = state.board;
    slot->side = state.side;
    slot->castling = state.castling;
    slot->en_passant = state.en_passant;
    slot->halfmove = state.halfmove;
    slot->fullmove = state.fullmove;
    slot->key = state.key;
    slot->piece_bitboards = state.piece_bitboards;
    slot->occupancy = state.occupancy;
    slot->occupied = state.occupied;
    slot->checkers = state.checkers;
    slot->pinned = state.pinned;
    slot->null_move = null_move;
    slot->repetition_history_fingerprint = state.repetition_history_fingerprint;
    slot->repetition_history_suppressed = state.repetition_history_suppressed;
}

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
    refresh_king_masks(state);
}

// The Zobrist key table is fetched once by apply_unchecked() and threaded
// through the incremental helpers, so a single make does not pay the
// function-local-static initialization guard per helper call.
void remove_derived_piece(
    NativeState& state, const ZobristKeys& keys, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    const std::uint64_t square_bit = bit(square);
    const std::size_t color = piece.color == Color::white ? 0 : 1;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] &= ~square_bit;
    state.occupancy[color] &= ~square_bit;
    state.key ^= keys.pieces[color][type][static_cast<std::size_t>(square)];
}

void add_derived_piece(
    NativeState& state, const ZobristKeys& keys, Piece piece, int square) noexcept {
    if (piece.empty() || !valid_square(square)) {
        return;
    }
    const std::uint64_t square_bit = bit(square);
    const std::size_t color = piece.color == Color::white ? 0 : 1;
    const std::size_t type = static_cast<std::size_t>(piece.type);
    state.piece_bitboards[color][type] |= square_bit;
    state.occupancy[color] |= square_bit;
    state.key ^= keys.pieces[color][type][static_cast<std::size_t>(square)];
}

void remove_rule_keys(NativeState& state, const ZobristKeys& keys) noexcept {
    state.key ^= keys.castling[state.castling & kAllCastling];
    if (has_legal_en_passant_capture_mutable(state)) {
        state.key ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) {
        state.key ^= keys.black_to_move;
    }
}

// `en_passant_capture_legal` carries apply_unchecked()'s probe result for
// this same rule state; re-probing here returned the identical value.
void add_rule_keys(
    NativeState& state, const ZobristKeys& keys, bool en_passant_capture_legal) noexcept {
    state.key ^= keys.castling[state.castling & kAllCastling];
    if (en_passant_capture_legal) {
        state.key ^= keys.en_passant[state.en_passant.index() & 7];
    }
    if (state.side == Color::black) {
        state.key ^= keys.black_to_move;
    }
}

int king_square(const NativeState& state, Color color) noexcept {
    const std::size_t color_index = color == Color::white ? 0U : 1U;
    const std::uint64_t kings =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
    if (kings == 0) return -1;
    return static_cast<int>(std::countr_zero(kings));
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
    const bool white = attacker == Color::white;
    const std::uint64_t occupied = state.occupied;

    // Pawns attack `target` from the two squares a pawn of `attacker` would
    // have moved from; the reverse table answers that with one lookup.
    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    if ((detail::pawn_attackers_of(target, white) & pawns) != 0) return true;

    // Knights and kings move without blocking, so their attack test is a pure
    // intersection of target-centric attack masks with the piece bitboards.
    if ((detail::knight_attacks(target) &
         state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::knight)]) != 0) {
        return true;
    }
    if ((detail::king_attacks(target) &
         state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)]) != 0) {
        return true;
    }

    // Sliding attacks are generated from the target through the real
    // occupancy, so the first blocker on each ray naturally stops the mask.
    const std::uint64_t bishops =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    if ((detail::bishop_attacks(target, occupied) & bishops) != 0) return true;
    const std::uint64_t rooks =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    if ((detail::rook_attacks(target, occupied) & rooks) != 0) return true;
    return false;
}

int attacker_count(const NativeState& state, int target, Color attacker) noexcept {
    if (!valid_square(target)) return 0;
    const std::size_t color_index = attacker == Color::white ? 0U : 1U;
    const bool white = attacker == Color::white;
    const std::uint64_t occupied = state.occupied;

    const std::uint64_t pawns =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::pawn)];
    const std::uint64_t knights =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::knight)];
    const std::uint64_t kings =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::king)];
    const std::uint64_t bishops =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t rooks =
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[color_index][static_cast<std::size_t>(PieceType::queen)];

    int count = static_cast<int>(
        std::popcount(detail::pawn_attackers_of(target, white) & pawns));
    count += static_cast<int>(std::popcount(detail::knight_attacks(target) & knights));
    count += static_cast<int>(std::popcount(detail::king_attacks(target) & kings));
    count += static_cast<int>(std::popcount(detail::bishop_attacks(target, occupied) & bishops));
    count += static_cast<int>(std::popcount(detail::rook_attacks(target, occupied) & rooks));
    return count;
}

bool is_checked(const NativeState& state, Color color) noexcept {
    const int king = king_square(state, color);
    return king < 0 || square_attacked(state, king, opposite(color));
}

// Recomputes the incremental king state for the side to move only: the
// checkers attacking its king and its own pieces pinned against an enemy
// slider.  apply_unchecked flips the side before callers ask about the mover,
// and every reader indexes the masks with the current side: the hoisted
// values in legal_moves, legal_moves_into_impl, and has_legal_move
// (position.cpp:962-964, 986-988, 1023-1025), the re-read in
// resolve_move_legality (position.cpp:878), whose callers pass state.side as
// the mover, and the side-to-move in_check() probe (position.cpp:1219).
// NativeState is private to this translation unit, so no other
// reader exists.  The other colour's masks are intentionally left stale: no
// reader touches them, and snapshot_into()/restore() copy both arrays
// (position.cpp:238, 1457), so unmake restores whatever was
// current when the snapshot was taken.
void refresh_king_masks(NativeState& state) noexcept {
    const std::size_t color_index = state.side == Color::white ? 0U : 1U;
    state.checkers[color_index] = 0;
    state.pinned[color_index] = 0;
    const Color color = state.side;
    const int king = king_square(state, color);
    if (king < 0) return;
    const std::size_t enemy_index = 1 - color_index;
    const bool enemy_pawns_are_white = color == Color::black;
    const std::uint64_t enemy_pawns =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::pawn)];
    const std::uint64_t enemy_knights =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::knight)];
    const std::uint64_t enemy_kings =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::king)];
    const std::uint64_t enemy_bishops =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::bishop)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];
    const std::uint64_t enemy_rooks =
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::rook)] |
        state.piece_bitboards[enemy_index][static_cast<std::size_t>(PieceType::queen)];

    const std::uint64_t checkers =
        (detail::pawn_attackers_of(king, enemy_pawns_are_white) & enemy_pawns) |
        (detail::knight_attacks(king) & enemy_knights) |
        (detail::king_attacks(king) & enemy_kings) |
        (detail::bishop_attacks(king, state.occupied) & enemy_bishops) |
        (detail::rook_attacks(king, state.occupied) & enemy_rooks);
    state.checkers[color_index] = checkers;

    // An enemy without a bishop/queen or rook/queen can never pin through
    // a blocker, so the scan below is skipped while `pinned` still holds
    // the zero written above.
    if (enemy_bishops == 0 && enemy_rooks == 0) {
        return;
    }

    // Pin scan.  Removing a piece can only expose a slider attack that
    // runs through the piece's square; a piece between the king and the
    // removed piece would still block, and a piece off the ray does not
    // affect it at all.  So a piece is pinned exactly when it is the
    // nearest blocker on one of the king's rays and the next piece on
    // that ray is an enemy slider of the matching type.  This is the same
    // predicate as the former per-piece trial, which removed the piece
    // from the occupancy and asked bishop_attacks/rook_attacks for an
    // enemy slider: removing the nearest blocker is the only occupancy
    // change that trial made, and sliding_attacks stops at the first
    // blocker, so its intersection is non-zero exactly when that next
    // blocker is an enemy bishop/queen on a diagonal ray or rook/queen on
    // an orthogonal ray.  attack_tables.cpp builds the direction order
    // N, NE, E, SE, S, SW, W, NW, so odd indices are the diagonals.
    std::uint64_t pinned = 0;
    for (int direction = 0; direction < detail::kAttackDirections; ++direction) {
        // One ray word answers both nearest-blocker scans.  A ray is
        // monotone in square index and starts on the square adjacent to the
        // king, so every blocker lies on one side of the king: the
        // lowest-index blocker sits above the king exactly on the
        // increasing-step rays (N, NE, E, NW) and below it on the four
        // decreasing ones.  That sign picks the near end for both scans.
        const std::uint64_t blockers =
            detail::ray_attacks(king, direction) & state.occupied;
        if (blockers == 0) continue;
        const int lowest = static_cast<int>(std::countr_zero(blockers));
        const int highest = 63 - static_cast<int>(std::countl_zero(blockers));
        const bool increasing = lowest > king;
        const int first = increasing ? lowest : highest;
        const Piece first_piece = state.board[static_cast<std::size_t>(first)];
        if (first_piece.color != color || first_piece.type == PieceType::king) continue;
        // Clearing the first blocker leaves the next blocker on the same
        // ray, which is exactly the occupancy the second nearest_blocker
        // call used to receive; picking its near end yields the same square.
        const std::uint64_t remaining = blockers & ~(std::uint64_t{1} << first);
        if (remaining == 0) continue;
        const int second = increasing ?
            static_cast<int>(std::countr_zero(remaining)) :
            63 - static_cast<int>(std::countl_zero(remaining));
        const Piece second_piece = state.board[static_cast<std::size_t>(second)];
        if (second_piece.color != opposite(color)) continue;
        const bool diagonal = (direction & 1) != 0;
        if (second_piece.type == PieceType::queen ||
            second_piece.type == (diagonal ? PieceType::bishop : PieceType::rook)) {
            pinned |= std::uint64_t{1} << first;
        }
    }
    state.pinned[color_index] = pinned;
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
        // Sliding-attack lookups read the occupancy bitboards, so the trial
        // capture has to update them as well; otherwise pinned-piece checks
        // would still see the captured pawn and the source square as occupied.
        state.occupancy[side_index] &= ~source_bit;
        state.occupancy[side_index] |= target_bit;
        state.occupancy[opposite_index] &= ~captured_bit;
        state.occupied = state.occupancy[0] | state.occupancy[1];
        const bool legal = !is_checked(state, side);
        state.board[static_cast<std::size_t>(source)] = pawn;
        state.board[static_cast<std::size_t>(captured_square)] = captured;
        state.board[static_cast<std::size_t>(target)] = {};
        state.piece_bitboards[side_index][pawn_index] |= source_bit;
        state.piece_bitboards[side_index][pawn_index] &= ~target_bit;
        state.piece_bitboards[opposite_index][pawn_index] |= captured_bit;
        state.occupancy[side_index] |= source_bit;
        state.occupancy[side_index] &= ~target_bit;
        state.occupancy[opposite_index] |= captured_bit;
        state.occupied = state.occupancy[0] | state.occupancy[1];
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
    // The read-only wrapper is reached from draw detection on every node, so
    // the overwhelmingly common "no en-passant target" case must not pay for
    // the full NativeState copy (board plus the history array).
    if (state.en_passant.index() >= Square::kInvalid) {
        return false;
    }
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
            // Positions whose non-moving side is already in check stay
            // accepted: they are unreachable in a real game, but composed
            // analysis boards and the project's own tactical fixtures use
            // them, and the search treats them as ordinary positions.
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

// Legality-probe filter for a pseudo-legal candidate.  The board and the
// king-state prologue fields are passed in by the caller instead of being
// re-read from `state` on every candidate.  That is exact because
// resolve_move_legality leaves them untouched: the king probe
// (king_move_is_legal) temporarily clears only `occupied` and restores it,
// and the non-king trial mutates only the captured/victim bitboard words and
// `occupied`, restoring them before returning.  resolve_move_legality never
// records history (snapshot_into belongs to the public make path), so the
// hoisted values stay loop-invariant.
[[nodiscard]] static bool move_requires_legality_probe(
    const Move& move, const Board& board,
    std::uint64_t checkers, std::uint64_t pinned, int en_passant) noexcept {
    const int from = move.from().index();
    const int to = move.to().index();
    if (!valid_square(from) || !valid_square(to)) {
        return true;
    }
    const Piece piece = board[static_cast<std::size_t>(from)];
    if (piece.type == PieceType::king) {
        return true;
    }
    if (checkers != 0) {
        return true;
    }
    if ((pinned & bit(from)) != 0) {
        return true;
    }
    if (piece.type == PieceType::pawn && en_passant < Square::kInvalid &&
        to == en_passant && board[static_cast<std::size_t>(to)].empty()) {
        return true;
    }
    return false;
}

    // King moves are decided directly: after the king vacates `from`, the
    // destination must not be attacked.  Castling is already fully validated
    // during generation (path clear, not in check, transit and destination
    // safe), so it needs no probe at all.  Clearing `from` from the occupancy
    // is what makes the ray the king steps along visible; without it a
    // checking slider behind the king would be missed.
    [[nodiscard]] bool king_move_is_legal(const Move& move) noexcept {
        const int from = move.from().index();
        const int to = move.to().index();
        if (!valid_square(from) || !valid_square(to)) {
            return false;
        }
        const int from_file = file_of(from);
        const int to_file = file_of(to);
        if (from_file - to_file == 2 || to_file - from_file == 2) {
            return true;
        }
        const std::uint64_t saved_occupied = state.occupied;
        state.occupied &= ~bit(from);
        const bool attacked = square_attacked(state, to, opposite(state.side));
        state.occupied = saved_occupied;
        return !attacked;
    }

    // Full legality resolution for a move the cheap filter could not accept:
    // king moves use the direct destination test; a non-king move in check is
    // first screened against the check, and everything else runs a minimal
    // in-place trial and checks the mover's king.
    [[nodiscard]] bool resolve_move_legality(const Move& move, Color mover) {
        const int from = move.from().index();
        const int to = move.to().index();
        if (valid_square(from) &&
            state.board[static_cast<std::size_t>(from)].type == PieceType::king) {
            return king_move_is_legal(move);
        }
        const std::size_t side_index = mover == Color::white ? 0U : 1U;
        const std::uint64_t checkers = state.checkers[side_index];
        if (checkers != 0) {
            // Only a capture of the single checker or an interposition on its
            // ray can answer a check with a non-king move; a double check can
            // only be answered by the king.  En passant is the one capture
            // whose target square is not the checker's square.
            if (!valid_square(to) || (checkers & (checkers - 1)) != 0) {
                return false;
            }
            if ((bit(to) & checkers) == 0) {
                const int king = king_square(state, mover);
                const int checker = static_cast<int>(std::countr_zero(checkers));
                bool answers = false;
                if (king >= 0) {
                    const std::uint64_t between =
                        (detail::bishop_attacks(king, checkers) &
                         detail::bishop_attacks(checker, bit(king))) |
                        (detail::rook_attacks(king, checkers) &
                         detail::rook_attacks(checker, bit(king)));
                    answers = (bit(to) & between) != 0;
                }
                if (!answers && valid_square(from) &&
                    state.board[static_cast<std::size_t>(from)].type == PieceType::pawn &&
                    state.en_passant.index() < Square::kInvalid &&
                    to == state.en_passant.index() &&
                    state.board[static_cast<std::size_t>(to)].empty()) {
                    const int captured = mover == Color::white ? to - 8 : to + 8;
                    answers = captured == checker;
                }
                if (!answers) {
                    return false;
                }
            }
        }
        // Minimal in-place trial.  is_checked() reads only `occupied` and the
        // piece bitboards: king_square() reads the mover's king word, and
        // square_attacked() reads the attacker's -- the opponent's -- pawn,
        // knight, king, bishop, queen, and rook words.  Castling never reaches
        // here (the king test above returns first) and the mover's king never
        // moves, so the only predicate-visible effects apply_unchecked() has
        // on a generated move are the captured piece and the en-passant victim
        // leaving the opponent's bitboards and `occupied` losing the from,
        // victim, and captured squares and gaining `to`.  Save exactly those
        // entries, apply those effects, probe, and restore them; board, keys,
        // side, rule state, and history stay untouched, so the caller's
        // hoisted board/checkers/pinned/en-passant values remain valid.  The
        // captured piece and the en-passant victim are always the opponent's:
        // the generator rejects own pieces and kings as targets, set_fen()
        // requires an enemy pawn behind an en-passant target, and
        // apply_unchecked() drops a target that has no legal capture.
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const Piece captured = state.board[static_cast<std::size_t>(to)];
        const bool en_passant = moving.type == PieceType::pawn &&
            to == state.en_passant.index() && captured.empty();
        const int victim_square =
            en_passant ? to + (moving.color == Color::white ? -8 : 8) : -1;
        const Piece victim = en_passant ?
            state.board[static_cast<std::size_t>(victim_square)] : Piece{};
        const std::size_t opponent_index = 1U - side_index;
        const std::size_t captured_type = static_cast<std::size_t>(captured.type);
        const std::size_t victim_type = static_cast<std::size_t>(victim.type);
        const std::uint64_t saved_captured_board =
            state.piece_bitboards[opponent_index][captured_type];
        const std::uint64_t saved_victim_board =
            state.piece_bitboards[opponent_index][victim_type];
        const std::uint64_t saved_occupied = state.occupied;
        if (!captured.empty()) {
            state.piece_bitboards[opponent_index][captured_type] &= ~bit(to);
        }
        if (en_passant) {
            state.piece_bitboards[opponent_index][victim_type] &= ~bit(victim_square);
        }
        state.occupied =
            (saved_occupied & ~bit(from) & ~bit(victim_square)) | bit(to);
        const bool legal = !is_checked(state, mover);
        state.occupied = saved_occupied;
        state.piece_bitboards[opponent_index][victim_type] = saved_victim_board;
        state.piece_bitboards[opponent_index][captured_type] = saved_captured_board;
        return legal;
    }

std::vector<Move> legal_moves() {
    const Color mover = state.side;
    const std::size_t side_index = mover == Color::white ? 0U : 1U;
    const std::uint64_t checkers = state.checkers[side_index];
    const std::uint64_t pinned = state.pinned[side_index];
    const int en_passant = state.en_passant.index();
    MoveBuffer pseudo;
    generate_pseudo(pseudo);
    std::vector<Move> result;
    result.reserve(pseudo.size());
    for (const Move& move : pseudo) {
        bool legal = !move_requires_legality_probe(
            move, state.board, checkers, pinned, en_passant);
        if (!legal) {
            legal = resolve_move_legality(move, mover);
        }
        if (legal) {
            result.push_back(move);
        }
    }
    return result;
}

    template <bool TacticalOnly>
    std::size_t legal_moves_into_impl(std::span<Move> output) {
        const Color mover = state.side;
        const std::size_t side_index = mover == Color::white ? 0U : 1U;
        const std::uint64_t checkers = state.checkers[side_index];
        const std::uint64_t pinned = state.pinned[side_index];
        const int en_passant = state.en_passant.index();
        MoveBuffer pseudo;
        generate_pseudo<TacticalOnly>(pseudo);
        std::size_t count = 0;
    for (const Move& move : pseudo) {
        bool legal = !move_requires_legality_probe(
            move, state.board, checkers, pinned, en_passant);
        if (!legal) {
            legal = resolve_move_legality(move, mover);
        }
        if (legal) {
            // Count every legal move; a too-small span only drops moves.
            if (count < output.size()) {
                output[count] = move;
            }
            ++count;
        }
    }
    return count;
    }

    std::size_t legal_moves_into(std::span<Move> output) {
        return legal_moves_into_impl<false>(output);
    }

    std::size_t legal_tactical_moves_into(std::span<Move> output) {
        return legal_moves_into_impl<true>(output);
    }

    // Stops at the first legal candidate instead of validating the whole
    // pseudo-legal list.  Quiescence only needs to know whether a legal reply
    // exists before deciding that a position is terminal.
    [[nodiscard]] bool has_legal_move() {
        const Color mover = state.side;
        const std::size_t side_index = mover == Color::white ? 0U : 1U;
        const std::uint64_t checkers = state.checkers[side_index];
        const std::uint64_t pinned = state.pinned[side_index];
        const int en_passant = state.en_passant.index();
        MoveBuffer pseudo;
        generate_pseudo(pseudo);
        for (const Move& move : pseudo) {
            bool legal = !move_requires_legality_probe(
                move, state.board, checkers, pinned, en_passant);
            if (!legal) {
                legal = resolve_move_legality(move, mover);
            }
            if (legal) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool is_legal(const Move& move) const noexcept {
        if (move.is_no_move()) {
            return false;
        }
        try {
            // Generate into a stack buffer instead of copying the whole
            // position and allocating a vector: this probe runs once per PV
            // move on the UCI reporting and completion paths.
            std::array<Move, kMaximumLegalMoves> legal{};
            const std::size_t count =
                const_cast<NativePosition*>(this)->legal_moves_into(legal);
            const Move* const begin = legal.data();
            const Move* const end = begin + count;
            return std::find(begin, end, move) != end;
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

    // Single-pass resolution of every board fact the metadata builders need.
    // The verdicts are exactly those of the individual accessors; callers that
    // need several of them for one move pay for one pass instead of six.
    [[nodiscard]] Position::MoveFacts move_facts(const Move& move) const noexcept {
        Position::MoveFacts facts;
        const std::uint8_t source = move.from().index();
        const std::uint8_t destination = move.to().index();
        if (source >= Square::kInvalid || destination >= Square::kInvalid) {
            return facts;
        }
        facts.valid = true;
        facts.from = state.board[source];
        facts.to = state.board[destination];
        facts.occupied_from = !facts.from.empty();
        facts.occupied_to = !facts.to.empty();
        facts.own_piece_on_from = facts.occupied_from && facts.from.color == state.side;
        // state.en_passant is kInvalid when no en-passant capture is
        // available, which no on-board destination can match.
        facts.en_passant_target = destination == state.en_passant.index();
        return facts;
    }

    bool apply_legal(const Move& move) {
        const auto legal = legal_moves();
        if (std::find(legal.begin(), legal.end(), move) == legal.end()) return false;
        snapshot_into(state);
        apply_unchecked(move);
        refresh_king_masks(state);
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
        if (move.from().index() >= Square::kInvalid || move.to().index() >= Square::kInvalid) {
            return false;
        }
        const Piece moving = state.board[move.from().index()];
        if (moving.empty() || moving.color != state.side) {
            return false;
        }
        snapshot_into(state);
        apply_unchecked(move);
        refresh_king_masks(state);
        return true;
    }

    bool unapply() noexcept {
        if (state.history_size == 0) return false;
        // Restore straight from the history slot: restore() only reads it,
        // and the slot is not written again until the next snapshot_into, so
        // the local copy was pure overhead.
        restore(state.history[--state.history_size]);
        return true;
    }

    bool unmake_move() noexcept {
        if (state.history_size == 0 || state.history[state.history_size - 1].null_move) {
            return false;
        }
        return unapply();
    }

    bool make_null_move() noexcept {
        snapshot_into(state, /*null_move=*/true);
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
        for (const auto& color_boards : state.piece_bitboards) {
            for (const std::uint64_t board : color_boards) {
                count += static_cast<std::size_t>(std::popcount(board));
            }
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

    [[nodiscard]] std::uint64_t occupied_squares() const noexcept { return state.occupied; }

    [[nodiscard]] bool in_check() const noexcept {
        // Bit-identity with is_checked(state, state.side): refresh_king_masks()
        // fills checkers[side] from exactly square_attacked()'s five terms, and
        // every public mutation refreshes the side-to-move mask (rebuild_derived
        // on set_fen and null transitions, apply_legal/make_generated_move after
        // the side flip, restore on unmake).  Board-mutating trial helpers
        // (has_legal_en_passant_capture_in_place, resolve_move_legality,
        // safe_king_step) call is_checked()/square_attacked() directly and never
        // read this mask.  is_checked() reports true for an absent king, so keep
        // that guard; set_fen requires one king per side, making it defensive.
        if (king_square(state, state.side) < 0) return true;
        return state.checkers[state.side == Color::white ? 0U : 1U] != 0;
    }

    [[nodiscard]] bool in_check(Color color) const noexcept { return is_checked(state, color); }

    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept {
        const std::size_t color_index = color == Color::white ? 0U : 1U;
        constexpr std::size_t knight = static_cast<std::size_t>(PieceType::knight);
        constexpr std::size_t bishop = static_cast<std::size_t>(PieceType::bishop);
        constexpr std::size_t rook = static_cast<std::size_t>(PieceType::rook);
        constexpr std::size_t queen = static_cast<std::size_t>(PieceType::queen);
        return (state.piece_bitboards[color_index][knight] |
                state.piece_bitboards[color_index][bishop] |
                state.piece_bitboards[color_index][rook] |
                state.piece_bitboards[color_index][queen]) != 0;
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
        // The shortest possible repetition cycle is four plies, so a shorter
        // halfmove clock makes the backward scan pointless.
        if (state.halfmove < 4) {
            return 1;
        }
        // A repeated key can only occur within the last `halfmove` plies:
        // an intervening pawn move, capture, or castling-right change would
        // have altered the key irreversibly, and the first two reset the
        // clock.  Clamped clocks (>= 255) keep the unbounded scan.
        std::size_t count = 1;
        const std::size_t scan = state.halfmove < 255
            ? std::min<std::size_t>(state.history_size, state.halfmove)
            : state.history_size;
        for (std::size_t index = state.history_size; index > state.history_size - scan; --index) {
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
            // has_legal_move() runs the same pseudo-generation and legality
            // filter as legal_moves(), with an early exit and no allocation.
            return in_check() && !mutable_this->has_legal_move();
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
        // Bitboard form: draw_status() reaches this on every node, and the
        // incrementally maintained piece bitboards answer it with a handful
        // of popcounts instead of a 64-square scan.
        constexpr std::size_t pawn = static_cast<std::size_t>(PieceType::pawn);
        constexpr std::size_t knight = static_cast<std::size_t>(PieceType::knight);
        constexpr std::size_t bishop = static_cast<std::size_t>(PieceType::bishop);
        constexpr std::size_t rook = static_cast<std::size_t>(PieceType::rook);
        constexpr std::size_t queen = static_cast<std::size_t>(PieceType::queen);
        constexpr std::uint64_t dark_squares = [] {
            std::uint64_t mask = 0;
            for (int square = 0; square < 64; ++square) {
                if ((((square & 7) + (square >> 3)) & 1) != 0) {
                    mask |= std::uint64_t{1} << square;
                }
            }
            return mask;
        }();
        if ((state.piece_bitboards[0][pawn] | state.piece_bitboards[1][pawn] |
             state.piece_bitboards[0][rook] | state.piece_bitboards[1][rook] |
             state.piece_bitboards[0][queen] | state.piece_bitboards[1][queen]) != 0) {
            return false;
        }
        const std::uint64_t knights =
            state.piece_bitboards[0][knight] | state.piece_bitboards[1][knight];
        const std::uint64_t bishops =
            state.piece_bitboards[0][bishop] | state.piece_bitboards[1][bishop];
        if (static_cast<int>(std::popcount(knights | bishops)) <= 1) {
            return true;
        }
        if (knights != 0) {
            return false;
        }
        // Only bishops remain: insufficient when every bishop stands on one
        // square color.
        return (bishops & dark_squares) == 0 || (bishops & ~dark_squares) == 0;
    }

    [[nodiscard]] bool is_known_locked_pawn_wall() const noexcept {
        // Keep dead-position recognition intentionally narrow. This blocked
        // pawn wall is a known FIDE dead-position example; the board is what
        // makes it dead, not the side to move or the bookkeeping fields in a
        // FEN. Compare the normalized board first: the pattern is almost
        // never present, and the en-passant certificate below needs a copy of
        // the whole state, so it must only run for a matching board.
        //
        // The pattern literal below holds exactly 14 non-empty squares
        // (17, 19, 20, 24, 25, 26, 28, 32, 34, 36, 39, 47, 50, 52), and the
        // board comparison accepts a position only when every pattern square
        // holds the same piece type and every other square is empty.  So
        // passing the loop implies exactly 14 occupied squares, and this
        // popcount pre-check is equivalent: it can only reject boards the
        // loop would reject too.  `state.occupied` is kept in sync with the
        // board by rebuild_derived and the make/unmake path.  The
        // en-passant exception is only reached once the board matched, so it
        // is unaffected.
        if (std::popcount(state.occupied) != 14) {
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
        // An actually legal en-passant capture is an exception because it
        // creates a pawn continuation from an otherwise locked wall.
        return !has_legal_en_passant_capture(state);
    }

    [[nodiscard]] bool is_dead_position() const noexcept {
        return is_insufficient_material() || is_known_locked_pawn_wall();
    }

    // Overload taking a precomputed repetition_count() so one history scan can
    // serve both the draw status and is_repetition_sensitive() for the same
    // unchanged position.  The branches, ordering, and verdicts are identical
    // to the scanning overload below; is_checkmate() stays lazily queried.
    [[nodiscard]] DrawStatus draw_status(std::size_t repetitions) const noexcept {
        if (is_dead_position()) return DrawStatus::dead_position;
        if (is_automatic_seventy_five_move_draw()) {
            return DrawStatus::automatic_seventy_five_move;
        }
        // The fivefold and threefold checks share the caller's history scan.
        if (repetitions >= 5 && !is_checkmate()) return DrawStatus::automatic_fivefold;
        if (can_claim_fifty_move_draw()) return DrawStatus::claimable_fifty_move;
        if (repetitions >= 3 && !is_checkmate()) return DrawStatus::claimable_threefold;
        return DrawStatus::none;
    }

    [[nodiscard]] DrawStatus draw_status() const noexcept {
        return draw_status(repetition_count());
    }

    [[nodiscard]] bool is_repetition_sensitive(std::size_t repetitions) const noexcept {
        return repetitions >= 2;
    }

    [[nodiscard]] bool is_repetition_sensitive() const noexcept {
        return is_repetition_sensitive(repetition_count());
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
    state.checkers = saved.checkers;
    state.pinned = saved.pinned;
}

    template <typename MoveContainer>
    void push(MoveContainer& moves, int from, int to) const {
        if (!valid_square(to)) return;
        push(moves, from, to, state.board[static_cast<std::size_t>(to)]);
    }

    // Bit-identity: `target` is the value the three-argument overload reloads
    // from `state.board[to]`; a caller that already loaded that same square
    // passes it here, so the checks, the promotion decision, and the emission
    // order below are unchanged.
    template <typename MoveContainer>
    void push(MoveContainer& moves, int from, int to, Piece target) const {
        if (!valid_square(to)) return;
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

    // `TacticalOnly` emits captures, en-passant, and promotions without the
    // quiet moves that quiescence would discard anyway.  The full generator
    // stays the default so perft and the public move list are unchanged.
    template <bool TacticalOnly = false, typename MoveContainer>
    void generate_pseudo(MoveContainer& moves) {
        const Color side = state.side;
        const std::size_t side_index = side == Color::white ? 0U : 1U;
        std::uint64_t origins = state.occupancy[side_index];
        while (origins != 0) {
            const int from = static_cast<int>(std::countr_zero(origins));
            origins &= origins - 1;
            const Piece piece = state.board[static_cast<std::size_t>(from)];
            if (piece.empty() || piece.color != side) continue;
            const int file = file_of(from);
            const int rank = rank_of(from);
            if (piece.type == PieceType::pawn) {
                const int direction = side == Color::white ? 1 : -1;
                const int one = from + direction * 8;
                if (valid_square(one)) {
                    // Bit-identity: the quiet target is loaded once here and
                    // the same Piece value is threaded into push(); the board
                    // cannot change between this load and either emission.
                    const Piece one_target = state.board[static_cast<std::size_t>(one)];
                    if (one_target.empty()) {
                        if constexpr (!TacticalOnly) {
                            push(moves, from, one, one_target);
                            const int two = from + direction * 16;
                            const int start_rank = side == Color::white ? 1 : 6;
                            if (rank == start_rank &&
                                state.board[static_cast<std::size_t>(two)].empty()) {
                                moves.emplace_back(
                                    Square::from_index(static_cast<std::uint8_t>(from)),
                                    Square::from_index(static_cast<std::uint8_t>(two)));
                            }
                        } else {
                            const int promotion_rank = side == Color::white ? 7 : 0;
                            if (rank_of(one) == promotion_rank) {
                                push(moves, from, one, one_target);
                            }
                        }
                    }
                }
                for (const int file_delta : {-1, 1}) {
                    const int target_file = file + file_delta;
                    const int target_rank = rank + direction;
                    if (target_file < 0 || target_file >= 8 || target_rank < 0 || target_rank >= 8) continue;
                    const int to = target_rank * 8 + target_file;
                    const Piece target = state.board[static_cast<std::size_t>(to)];
                    // Bit-identity: `target` is the Piece the three-argument
                    // push() reloaded; the board is not mutated before the
                    // call, including the empty en-passant target square.
                    if ((!target.empty() && target.color != side && target.type != PieceType::king) ||
                        (state.en_passant.index() == to && target.empty())) {
                        push(moves, from, to, target);
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
                        const int to = target_rank * 8 + target_file;
                        // Bit-identity: the target square is loaded once and
                        // the same Piece value is threaded into push() in both
                        // generator modes; the checks and order are unchanged.
                        const Piece target = state.board[static_cast<std::size_t>(to)];
                        if constexpr (TacticalOnly) {
                            if (target.empty() || target.color == side ||
                                target.type == PieceType::king) {
                                continue;
                            }
                        }
                        push(moves, from, to, target);
                    }
                }
                if constexpr (!TacticalOnly) {
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
                            if constexpr (!TacticalOnly) {
                                moves.emplace_back(Square::from_index(static_cast<std::uint8_t>(from)),
                                                   Square::from_index(static_cast<std::uint8_t>(to)));
                            }
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
        // One guarded fetch for the whole make: every incremental helper
        // below would otherwise pay the function-local static guard on its
        // own zobrist() call, and the table never changes mid-make.
        const ZobristKeys& keys = zobrist();
        const Piece moving = state.board[static_cast<std::size_t>(from)];
        const Piece captured = state.board[static_cast<std::size_t>(to)];
        const bool pawn_move = moving.type == PieceType::pawn;
        const bool castling = moving.type == PieceType::king && std::abs(file_of(to) - file_of(from)) == 2;
        const bool en_passant = pawn_move && to == state.en_passant.index() && captured.empty();
        const bool capture = !captured.empty() || en_passant;

        // Remove the old rule-state keys and every piece that is about to
        // leave the board before mutating the board array. The remaining
        // derived state is rebuilt only from the pieces that actually moved.
        remove_rule_keys(state, keys);
        remove_derived_piece(state, keys, moving, from);
        if (!captured.empty()) {
            remove_derived_piece(state, keys, captured, to);
        }
        const int en_passant_captured_square =
            en_passant ? to + (moving.color == Color::white ? -8 : 8) : -1;
        if (en_passant) {
            remove_derived_piece(
                state, keys, state.board[static_cast<std::size_t>(en_passant_captured_square)],
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
        add_derived_piece(state, keys, state.board[static_cast<std::size_t>(to)], to);
        if (castling) {
            const int rook_from = to > from ? from + 3 : from - 4;
            const int rook_to = to > from ? from + 1 : from - 1;
            const Piece rook = state.board[static_cast<std::size_t>(rook_from)];
            remove_derived_piece(state, keys, rook, rook_from);
            state.board[static_cast<std::size_t>(rook_to)] = rook;
            state.board[static_cast<std::size_t>(rook_from)] = {};
            add_derived_piece(state, keys, rook, rook_to);
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
        // Bit-identity: add_rule_keys() used to re-run this probe on the same
        // rule state; the only field rewritten in between is `occupied`,
        // which the probe rebuilds from the occupancy bitboards before its
        // first read.  On a failed probe the cleared en-passant square makes
        // the former re-probe return false too, so the result matches it.
        const bool en_passant_capture_legal =
            state.en_passant.index() < Square::kInvalid &&
            has_legal_en_passant_capture_mutable(state);
        if (!en_passant_capture_legal) {
            state.en_passant = {};
        }
        state.occupied = state.occupancy[0] | state.occupancy[1];
        add_rule_keys(state, keys, en_passant_capture_legal);
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

void Position::copy_board_to(std::span<Piece, 64> output) const noexcept {
    std::ranges::copy(impl_->position.state.board, output.begin());
}

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

std::size_t Position::legal_tactical_moves_into(std::span<Move> output) const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).legal_tactical_moves_into(output);
    } catch (...) {
        return 0;
    }
}

bool Position::has_legal_move() const noexcept {
    try {
        return const_cast<NativePosition&>(impl_->position).has_legal_move();
    } catch (...) {
        return false;
    }
}

bool Position::is_legal(const Move& move) const noexcept { return impl_->position.is_legal(move); }

bool Position::is_capture(const Move& move) const noexcept { return impl_->position.is_capture(move); }

Position::MoveFacts Position::move_facts(const Move& move) const noexcept {
    return impl_->position.move_facts(move);
}

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

std::uint64_t Position::occupied_squares() const noexcept {
    return impl_->position.occupied_squares();
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

DrawStatus Position::draw_status(std::size_t repetitions) const noexcept {
    return impl_->position.draw_status(repetitions);
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
