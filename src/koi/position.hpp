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
    [[nodiscard]] std::vector<Move> legal_moves() const;
    // Allocation-free legal generation for hot callers. The output preserves
    // the same deterministic ordering as legal_moves(); entries beyond the
    // returned count are untouched.
    [[nodiscard]] std::size_t legal_moves_into(std::span<Move> output) const noexcept;
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
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
    [[nodiscard]] std::size_t piece_count() const noexcept;
    [[nodiscard]] std::uint64_t piece_bitboard(PieceType type, Color color) const noexcept;
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
    [[nodiscard]] bool is_repetition_sensitive() const noexcept;
    [[nodiscard]] bool is_draw_by_rule() const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
