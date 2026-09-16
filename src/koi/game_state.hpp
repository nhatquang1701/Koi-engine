#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "koi/move.hpp"

namespace koi {

class GameState;

enum class PositionErrorCode : std::uint8_t { malformed_fen, illegal_position };

struct PositionError {
    PositionErrorCode code;
    std::string message;
};

// Claimable outcomes are reported separately from automatic terminal draws.
// The UCI layer still returns a legal move when a claim is available.
enum class DrawStatus : std::uint8_t {
    none,
    claimable_threefold,
    claimable_fifty_move,
    automatic_fivefold,
    automatic_seventy_five_move,
    dead_position,
};

enum class MoveKind : std::uint8_t { quiet, capture, en_passant, castling, promotion };

enum class CheckFlagMode : std::uint8_t {
    all_moves,
    quiet_moves_only,
};

struct MoveMetadata {
    Move move;
    PieceType moving_piece = PieceType::none;
    PieceType captured_piece = PieceType::none;
    MoveKind kind = MoveKind::quiet;
    bool gives_check = false;
    bool see_computed = false;
    std::int16_t see_score = 0;
    std::int32_t ordering_score = 0;
    // A generated metadata record is valid only for this exact position.
    // Keeping the key here lets the fast make path reject stale records
    // without rebuilding a native legal-move list.
    std::uint64_t position_key = 0;
    // Internal provenance token.  It is deliberately not part of the public
    // UCI surface; zero means the record was not produced by Koi's generator.
    std::uint64_t validation_token = 0;

    [[nodiscard]] constexpr bool is_capture() const noexcept {
        return kind == MoveKind::capture || kind == MoveKind::en_passant;
    }
};

namespace detail {
[[nodiscard]] int static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;
}

inline constexpr std::size_t kMaximumLegalMoves = 256;

class MoveMetadataList {
public:
    using iterator = std::array<MoveMetadata, kMaximumLegalMoves>::iterator;
    using const_iterator = std::array<MoveMetadata, kMaximumLegalMoves>::const_iterator;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr MoveMetadata& front() noexcept { return storage_[0]; }
    [[nodiscard]] constexpr const MoveMetadata& front() const noexcept { return storage_[0]; }
    [[nodiscard]] constexpr MoveMetadata& back() noexcept { return storage_[size_ - 1]; }
    [[nodiscard]] constexpr const MoveMetadata& back() const noexcept { return storage_[size_ - 1]; }
    [[nodiscard]] constexpr MoveMetadata& operator[](std::size_t index) noexcept { return storage_[index]; }
    [[nodiscard]] constexpr const MoveMetadata& operator[](std::size_t index) const noexcept {
        return storage_[index];
    }
    [[nodiscard]] constexpr iterator begin() noexcept { return storage_.begin(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return storage_.begin(); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return storage_.cbegin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return storage_.begin() + size_; }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return storage_.begin() + size_; }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return storage_.cbegin() + size_; }

    constexpr void clear() noexcept { size_ = 0; }

    [[nodiscard]] constexpr bool push_back(const MoveMetadata& metadata) noexcept {
        if (size_ >= kMaximumLegalMoves) {
            return false;
        }
        storage_[size_++] = metadata;
        return true;
    }

    constexpr void resize(std::size_t size) noexcept {
        size_ = std::min(size, kMaximumLegalMoves);
    }

private:
    std::array<MoveMetadata, kMaximumLegalMoves> storage_{};
    std::size_t size_ = 0;
};

struct PositionFeatures {
    std::array<Piece, 64> board{};
    std::array<std::uint64_t, 2> attacked_squares{};
    std::array<std::uint16_t, 2> mobility{};
    std::array<Square, 2> king_squares{};
    std::array<std::uint8_t, 2> pawn_file_masks{};
    std::array<std::uint8_t, 2> development{};
    std::array<std::uint8_t, 2> center_control{};
    std::array<std::uint8_t, 2> king_zone_attacks{};
    std::uint8_t castling_rights = 0;
    std::uint8_t game_phase = 0;
    std::uint16_t fullmove_number = 1;
    Color side_to_move = Color::white;
};

// Diagnostic-only state comparison data.  The native position remains the
// production authority; this snapshot exists to prove that the compatibility
// mirror has not drifted while it is still used by legacy consumers.
struct PositionConsistencySnapshot {
    std::string native_fen;
    std::string shadow_fen;
    std::vector<std::string> native_legal_moves;
    std::vector<std::string> shadow_legal_moves;
    std::uint64_t native_position_key = 0;
    std::uint64_t shadow_position_key = 0;
    std::uint8_t native_castling_rights = 0;
    std::uint8_t shadow_castling_rights = 0;
    Square native_en_passant_square{};
    Square shadow_en_passant_square{};
    std::uint16_t native_halfmove_clock = 0;
    std::uint16_t shadow_halfmove_clock = 0;
    std::uint16_t native_fullmove_number = 0;
    std::uint16_t shadow_fullmove_number = 0;
    std::size_t native_repetition_count = 0;
    std::size_t shadow_repetition_count = 0;
    bool native_repetition_sensitive = false;
    bool shadow_repetition_sensitive = false;
    bool native_can_claim_threefold_repetition = false;
    bool shadow_can_claim_threefold_repetition = false;
    bool native_is_automatic_fivefold_repetition = false;
    bool shadow_is_automatic_fivefold_repetition = false;
    bool native_in_check = false;
    bool shadow_in_check = false;
    Color native_side_to_move = Color::white;
    Color shadow_side_to_move = Color::white;

    [[nodiscard]] bool operator==(const PositionConsistencySnapshot&) const = default;

    [[nodiscard]] bool consistent() const noexcept {
        return native_fen == shadow_fen &&
            native_legal_moves == shadow_legal_moves &&
            native_castling_rights == shadow_castling_rights &&
            native_en_passant_square == shadow_en_passant_square &&
            native_halfmove_clock == shadow_halfmove_clock &&
            native_fullmove_number == shadow_fullmove_number &&
            native_repetition_count == shadow_repetition_count &&
            native_repetition_sensitive == shadow_repetition_sensitive &&
            native_can_claim_threefold_repetition == shadow_can_claim_threefold_repetition &&
            native_is_automatic_fivefold_repetition == shadow_is_automatic_fivefold_repetition &&
            native_in_check == shadow_in_check &&
            native_side_to_move == shadow_side_to_move;
    }
};

inline constexpr std::uint8_t kWhiteKingSideCastling = 0x1;
inline constexpr std::uint8_t kWhiteQueenSideCastling = 0x2;
inline constexpr std::uint8_t kBlackKingSideCastling = 0x4;
inline constexpr std::uint8_t kBlackQueenSideCastling = 0x8;
inline constexpr std::uint8_t kAllCastlingRights =
    kWhiteKingSideCastling | kWhiteQueenSideCastling |
    kBlackKingSideCastling | kBlackQueenSideCastling;

struct TablebaseSnapshot {
    std::array<std::array<std::uint64_t, 6>, 2> piece_bitboards{};
    Color side_to_move = Color::white;
    Square en_passant_square{};
    std::uint16_t halfmove_clock = 0;
    std::uint8_t castling_rights = 0;

    [[nodiscard]] std::size_t piece_count() const noexcept;
};

class GameState {
public:
    GameState();
    GameState(const GameState&);
    GameState(GameState&&) noexcept;
    GameState& operator=(const GameState&);
    GameState& operator=(GameState&&) noexcept;
    ~GameState();

    [[nodiscard]] static GameState startpos();
    [[nodiscard]] static std::expected<GameState, PositionError> from_fen(std::string_view fen);
    [[nodiscard]] std::string fen() const;
    [[nodiscard]] Color side_to_move() const noexcept;
    [[nodiscard]] Piece piece_at(Square square) const noexcept;
    [[nodiscard]] std::vector<Move> legal_moves() const;
    [[nodiscard]] PositionConsistencySnapshot consistency_snapshot() const;
    // Cheap search-boundary mirror check. The exhaustive consistency snapshot
    // remains available for tests and incident reports.
    [[nodiscard]] bool native_shadow_consistent() const noexcept;
    [[nodiscard]] std::vector<MoveMetadata> legal_moves_with_metadata() const;
    void legal_moves_with_metadata(MoveMetadataList& moves,
                                   bool include_check_flags = true,
                                   bool include_see = true,
                                   CheckFlagMode check_flag_mode = CheckFlagMode::all_moves) const noexcept;
    // Generates all legal evasions while checked, or only captures,
    // promotions, and checking moves otherwise. The return value reports
    // whether any legal move exists, even when a quiet non-checking move was
    // intentionally omitted from the output.
    [[nodiscard]] bool legal_tactical_moves_with_metadata(
        MoveMetadataList& moves, bool include_quiet_checks = true,
        bool include_see = true,
        CheckFlagMode check_flag_mode = CheckFlagMode::all_moves) const noexcept;
    [[nodiscard]] std::optional<MoveMetadata> describe_move(const Move&) const noexcept;
    [[nodiscard]] PositionFeatures position_features() const noexcept;
    // Diagnostic-only count of cache rebuilds. Search uses this to verify that
    // make/unmake restores parent feature snapshots instead of rebuilding them.
    [[nodiscard]] std::uint64_t position_feature_cache_misses() const noexcept;
    [[nodiscard]] TablebaseSnapshot tablebase_snapshot() const noexcept;
    [[nodiscard]] bool is_legal(const Move& move) const noexcept;
    bool make_move(const Move& move) noexcept;
    // Validates metadata against both native and shadow legality authorities.
    bool make_legal_move(const MoveMetadata& metadata) noexcept;
    // Fast transactional path for metadata returned by
    // legal_moves_with_metadata() for this unchanged position. Native legal
    // generation is authoritative; the shadow board is still updated and
    // compared after the move so divergence rolls the transaction back.
    bool make_generated_move(const MoveMetadata& metadata) noexcept;
    // Search-only fast path. Native legality remains authoritative and the
    // compatibility board is still updated transactionally; the mirror check
    // is performed at search boundaries to avoid repeating the same 12-piece
    // comparison at every interior node.
    bool make_search_move(const MoveMetadata& metadata) noexcept;
    // Search-only: stop maintaining the vendored compatibility board for this
    // state and every state copied from it. Search never reads the shadow, so
    // detaching removes a second board copy from each interior make/unmake.
    // Public API states must keep mirror tracking enabled because legacy
    // consumers (Polyglot, diagnostics, shadow verification) rely on it.
    void detach_mirror() noexcept;
    bool unmake_move() noexcept;
    bool make_null_move() noexcept;
    bool unmake_null_move() noexcept;
    [[nodiscard]] bool is_capture(const Move& move) const noexcept;
    // Fast native check probe used by the search when regular move metadata
    // intentionally omits capture check flags.
    [[nodiscard]] bool move_gives_check(const Move& move) const noexcept;
    [[nodiscard]] bool in_check() const noexcept;
    [[nodiscard]] bool in_check(Color color) const noexcept;
    [[nodiscard]] bool has_non_pawn_material(Color color) const noexcept;
    // True when the current position has already occurred once in the
    // reversible move history. Null-move pruning must treat this twofold
    // state conservatively because a null move can create a false cutoff.
    [[nodiscard]] bool is_repetition_sensitive() const noexcept;
    // Draw conditions that can be checked after legal move generation. A
    // caller that needs checkmate/stalemate must still inspect legal_moves().
    [[nodiscard]] bool is_draw_by_rule() const noexcept;
    [[nodiscard]] bool is_terminal() const noexcept;
    [[nodiscard]] std::uint64_t position_key() const noexcept;
    // Search-only fingerprint of reversible ancestor position keys. It is
    // intentionally separate from position_key(), which remains the public
    // board/repetition identity used by UCI and search-session cancellation.
    [[nodiscard]] std::uint64_t repetition_history_fingerprint() const noexcept;
    // Search-only marker for a speculative null-move branch. It prevents
    // artificial null descendants from sharing regular TT bounds with legal
    // positions that happen to have the same board and rule-clock values.
    [[nodiscard]] bool repetition_history_suppressed() const noexcept;
    // Search-only structural key for the adaptive pawn-history table.
    [[nodiscard]] std::uint64_t pawn_key() const noexcept;
    [[nodiscard]] std::uint64_t polyglot_key() const noexcept;
    [[nodiscard]] std::uint8_t castling_rights() const noexcept;
    [[nodiscard]] Square en_passant_square() const noexcept;
    [[nodiscard]] std::uint16_t halfmove_clock() const noexcept;
    [[nodiscard]] std::uint16_t fullmove_number() const noexcept;
    [[nodiscard]] std::size_t repetition_count() const noexcept;
    [[nodiscard]] bool can_claim_threefold_repetition() const noexcept;
    [[nodiscard]] bool can_claim_fifty_move_draw() const noexcept;
    [[nodiscard]] bool is_automatic_fivefold_repetition() const noexcept;
    [[nodiscard]] bool is_automatic_seventy_five_move_draw() const noexcept;
    [[nodiscard]] bool is_dead_position() const noexcept;
    [[nodiscard]] DrawStatus draw_status() const noexcept;
    // A claimable draw is an option for the side to move, not an automatic
    // terminal result. Search must still consider legal continuations.
    [[nodiscard]] bool is_claimable_draw() const noexcept;
    // Dead positions and automatic rule draws end the game immediately.
    [[nodiscard]] bool is_forced_draw() const noexcept;

private:
    [[nodiscard]] int direct_static_exchange_gain(const MoveMetadata&) const noexcept;
    [[nodiscard]] std::optional<MoveMetadata> metadata_for_native_move(
        const Move&, bool include_check_flags, CheckFlagMode check_flag_mode) const noexcept;
    bool apply_generated_move(const MoveMetadata&, bool verify_shadow_legality,
                              bool verify_mirror) noexcept;
    void finalize_metadata(MoveMetadataList&, std::uint64_t position_key,
                           bool include_see,
                           const PositionFeatures* exchange_features = nullptr) const noexcept;
    void invalidate_feature_cache() noexcept;

    friend int detail::static_exchange_gain(const GameState&, const MoveMetadata&) noexcept;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
