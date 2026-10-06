#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "koi/move.hpp"

namespace koi {

enum class DrawStatus : std::uint8_t;

class Position {
public:
    Position();
    explicit Position(std::string_view fen);
    Position(const Position&);
    Position(Position&&) noexcept;
    Position& operator=(const Position&);
    Position& operator=(Position&&) noexcept;
    ~Position();

    [[nodiscard]] std::string fen() const;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] Piece piece_at(Square square) const noexcept;
    // Copies the native mailbox into `output` in square-index order (one block
    // copy). Bulk callers such as SEE context construction avoid 64 cross-TU
    // piece_at calls per snapshot.
    void copy_board_to(std::span<Piece, 64> output) const noexcept;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    // Allocation-free legal generation for hot callers. The output preserves
    // the same deterministic ordering as legal_moves(); the returned value is
    // the total legal move count, and a span smaller than that count receives
    // only its first entries (internal callers always pass the 256-move
    // maximum, so truncation only affects external users).
    [[nodiscard]] std::size_t legal_moves_into(std::span<Move> output) const noexcept;
    // Tactical-only variant for quiescence: captures, en-passant, and
    // promotions, in the same relative order as legal_moves_into().
    [[nodiscard]] std::size_t legal_tactical_moves_into(std::span<Move> output) const noexcept;
    // True as soon as one legal move is found, without generating the rest.
    [[nodiscard]] bool has_legal_move() const noexcept;
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
    // Per-move board facts resolved in one pass: the origin and destination
    // occupants, the en-passant target comparison, and the capture verdict.
    // Hot metadata callers need all of them for the same move, so resolving
    // them once replaces up to six independent piece/en-passant lookups.
    struct MoveFacts {
        bool valid = false;              // endpoints are on-board and not a null move
        bool occupied_from = false;      // the origin square holds a piece
        bool occupied_to = false;        // the destination square holds a piece
        bool own_piece_on_from = false;  // the origin piece belongs to the side to move
        bool en_passant_target = false;  // the destination is the en-passant target square
        Piece from{};
        Piece to{};

        // Identical verdict to is_capture() for the move these facts describe.
        [[nodiscard]] constexpr bool is_capture() const noexcept {
            return own_piece_on_from &&
                ((!to.empty() && to.color != from.color) ||
                 (from.type == PieceType::pawn && to.empty() && en_passant_target));
        }
    };
    [[nodiscard]] MoveFacts move_facts(const Move& move) const noexcept;
    bool make_move(const Move& move) noexcept;
    // Applies a move that came from this position's legal move generator.
    // Callers must not use this for unvalidated external input.
    bool make_generated_move(const Move& move) noexcept;
    bool unmake_move() noexcept;
    bool make_null_move() noexcept;
    bool unmake_null_move() noexcept;
    bool apply_uci(std::string_view uci);
    bool unapply();
    bool set_fen(std::string_view fen);
    // Fixture-only parser for synthetic perft/rules positions. It keeps the
    // board structurally parseable but does not enforce strict material,
    // check-state, or king-distance legality rules used by production UCI.
    bool set_fen_unchecked(std::string_view fen);
    [[nodiscard]] std::uint64_t position_key() const noexcept;
    // Search-only fingerprint of reversible ancestor position keys since the
    // most recent pawn move, capture, or castling-rights change. Null moves
    // do not contribute to this fingerprint, and move unmake restores it.
    [[nodiscard]] std::uint64_t repetition_history_fingerprint() const noexcept;
    // Search-only marker for the speculative null-move branch. Such a branch
    // suppresses legal repetition and move-clock draws until an irreversible
    // move starts a fresh real-history segment, so it must not share a search
    // bound with an otherwise identical legal position.
    [[nodiscard]] bool repetition_history_suppressed() const noexcept;
    // Key containing only the two pawn bitboards. Search history tables use
    // it so equal pawn structures share experience across otherwise
    // different piece placements and rule-state fields.
    [[nodiscard]] std::uint64_t pawn_key() const noexcept;
    [[nodiscard]] std::size_t piece_count() const noexcept;
    [[nodiscard]] std::uint64_t piece_bitboard(PieceType type, Color color) const noexcept;
    // Combined occupancy of both colors, maintained by native make/unmake.
    [[nodiscard]] std::uint64_t occupied_squares() const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept;
    [[nodiscard]] std::uint8_t castling_rights() const noexcept;
    [[nodiscard]] Square en_passant_square() const noexcept;
    [[nodiscard]] std::size_t repetition_count() const noexcept;
    [[nodiscard]] bool can_claim_threefold_repetition() const noexcept;
    [[nodiscard]] bool can_claim_fifty_move_draw() const noexcept;
    [[nodiscard]] bool is_automatic_fivefold_repetition() const noexcept;
    [[nodiscard]] bool is_automatic_seventy_five_move_draw() const noexcept;
    [[nodiscard]] bool is_dead_position() const noexcept;
    [[nodiscard]] DrawStatus draw_status() const noexcept;
    // Overload taking a precomputed repetition_count() so one history scan can
    // serve both the draw status and repetition sensitivity of the same
    // unchanged position.
    [[nodiscard]] DrawStatus draw_status(std::size_t repetitions) const noexcept;
    // A claimable draw is an option for the side to move, not an automatic
    // terminal result. Search must still consider legal continuations.
    [[nodiscard]] bool is_claimable_draw() const noexcept;
    // Dead positions and automatic rule draws end the game immediately.
    [[nodiscard]] bool is_forced_draw() const noexcept;
    [[nodiscard]] bool is_repetition_sensitive() const noexcept;
    [[nodiscard]] bool is_draw_by_rule() const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept;
    // Native rule-history depth (one entry per applied move). Feature caches
    // are keyed by this instead of any adapter depth so the cache index stays
    // owned by the native authority.
    [[nodiscard]] std::size_t history_size() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
