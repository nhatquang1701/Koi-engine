#include "koi/syzygy_tablebase.hpp"

#include <algorithm>
#include <bit>
#include <limits>

#include <tbprobe.h>

namespace koi {

namespace {

std::mutex& fathom_mutex() {
    static std::mutex mutex;
    return mutex;
}

struct FathomOwnership {
    std::filesystem::path path;
    std::size_t users = 0;
};

FathomOwnership& fathom_ownership() {
    static FathomOwnership ownership;
    return ownership;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

bool acquire_fathom(const std::filesystem::path& path) {
    std::lock_guard lock(fathom_mutex());
    FathomOwnership& ownership = fathom_ownership();
    const std::filesystem::path normalized = normalized_path(path);
    if (ownership.users != 0) {
        if (ownership.path != normalized) {
            return false;
        }
        ++ownership.users;
        return true;
    }

    const bool initialized = tb_init(normalized.string().c_str());
    if (!initialized || TB_LARGEST == 0) {
        if (initialized) {
            tb_free();
        }
        return false;
    }
    ownership.path = normalized;
    ownership.users = 1;
    return true;
}

void release_fathom() noexcept {
    std::lock_guard lock(fathom_mutex());
    FathomOwnership& ownership = fathom_ownership();
    if (ownership.users == 0) {
        return;
    }
    --ownership.users;
    if (ownership.users == 0) {
        tb_free();
        ownership.path.clear();
    }
}

std::optional<Move> move_from_fathom(const TbMove move) noexcept {
    const Square from = Square::from_index(static_cast<std::uint8_t>(TB_MOVE_FROM(move)));
    const Square to = Square::from_index(static_cast<std::uint8_t>(TB_MOVE_TO(move)));
    Promotion promotion = Promotion::none;
    switch (TB_MOVE_PROMOTES(move)) {
    case TB_PROMOTES_KNIGHT: promotion = Promotion::knight; break;
    case TB_PROMOTES_BISHOP: promotion = Promotion::bishop; break;
    case TB_PROMOTES_ROOK: promotion = Promotion::rook; break;
    case TB_PROMOTES_QUEEN: promotion = Promotion::queen; break;
    case TB_PROMOTES_NONE: break;
    default: return std::nullopt;
    }
    return Move{from, to, promotion};
}

void native_bitboards(const TablebaseSnapshot& snapshot, std::uint64_t& white,
                      std::uint64_t& black, std::uint64_t& kings, std::uint64_t& queens,
                      std::uint64_t& rooks, std::uint64_t& bishops, std::uint64_t& knights,
                      std::uint64_t& pawns) noexcept {
    white = black = kings = queens = rooks = bishops = knights = pawns = 0;
    for (std::size_t color = 0; color < 2; ++color) {
        const std::uint64_t color_bits = snapshot.piece_bitboards[color][0] |
            snapshot.piece_bitboards[color][1] | snapshot.piece_bitboards[color][2] |
            snapshot.piece_bitboards[color][3] | snapshot.piece_bitboards[color][4] |
            snapshot.piece_bitboards[color][5];
        if (color == 0) white = color_bits; else black = color_bits;
        kings |= snapshot.piece_bitboards[color][5];
        queens |= snapshot.piece_bitboards[color][4];
        rooks |= snapshot.piece_bitboards[color][3];
        bishops |= snapshot.piece_bitboards[color][2];
        knights |= snapshot.piece_bitboards[color][1];
        pawns |= snapshot.piece_bitboards[color][0];
    }
}

unsigned native_ep(const TablebaseSnapshot& snapshot) noexcept {
    return snapshot.en_passant_square.index() < Square::kInvalid ?
        static_cast<unsigned>(snapshot.en_passant_square.index()) : 0;
}

bool valid_snapshot(const TablebaseSnapshot& snapshot) noexcept {
    if (snapshot.side_to_move != Color::white && snapshot.side_to_move != Color::black) {
        return false;
    }

    std::uint64_t occupied = 0;
    for (const auto& colors : snapshot.piece_bitboards) {
        for (const std::uint64_t pieces : colors) {
            if ((occupied & pieces) != 0) {
                return false;
            }
            occupied |= pieces;
        }
    }

    const std::uint64_t white_kings =
        snapshot.piece_bitboards[0][static_cast<std::size_t>(PieceType::king) - 1];
    const std::uint64_t black_kings =
        snapshot.piece_bitboards[1][static_cast<std::size_t>(PieceType::king) - 1];
    if (std::popcount(white_kings) != 1 || std::popcount(black_kings) != 1) {
        return false;
    }
    const int white_king = std::countr_zero(white_kings);
    const int black_king = std::countr_zero(black_kings);
    const int file_delta = (white_king & 7) - (black_king & 7);
    const int rank_delta = (white_king >> 3) - (black_king >> 3);
    if (file_delta >= -1 && file_delta <= 1 && rank_delta >= -1 && rank_delta <= 1) {
        return false;
    }

    constexpr std::uint64_t kBackRanks = 0xFF000000000000FFULL;
    const std::uint64_t pawns = snapshot.piece_bitboards[0][0] | snapshot.piece_bitboards[1][0];
    if ((pawns & kBackRanks) != 0) {
        return false;
    }

    if (snapshot.en_passant_square.index() >= Square::kInvalid) {
        return true;
    }
    if (snapshot.halfmove_clock != 0) {
        return false;
    }

    const int target = snapshot.en_passant_square.index();
    const bool white_to_move = snapshot.side_to_move == Color::white;
    if ((white_to_move && (target >> 3) != 5) || (!white_to_move && (target >> 3) != 2)) {
        return false;
    }
    const std::uint64_t target_bit = std::uint64_t{1} << target;
    if ((occupied & target_bit) != 0) {
        return false;
    }

    const int pawn_square = target + (white_to_move ? -8 : 8);
    const int origin_square = target + (white_to_move ? 8 : -8);
    if (pawn_square < 0 || pawn_square >= 64 || origin_square < 0 || origin_square >= 64) {
        return false;
    }
    const std::size_t pawn_color = white_to_move ? 1 : 0;
    const std::uint64_t pawn_bit = std::uint64_t{1} << pawn_square;
    const std::uint64_t origin_bit = std::uint64_t{1} << origin_square;
    return (snapshot.piece_bitboards[pawn_color][0] & pawn_bit) != 0 &&
        (occupied & origin_bit) == 0;
}

} // namespace

class SyzygyTablebase::Impl {
public:
    std::filesystem::path path;
    std::uint8_t probe_limit = 5;
    std::uint8_t probe_depth = 1;
    bool fifty_move_rule = true;
    bool enabled = false;
    std::atomic<std::uint64_t> hits = 0;
};

SyzygyScore syzygy_score(const SyzygyWdl wdl) noexcept {
    switch (wdl) {
    case SyzygyWdl::win:
        return {100'000 - 1, 1};
    case SyzygyWdl::loss:
        return {-100'000 + 1, -1};
    case SyzygyWdl::cursed_win:
        return {1, std::nullopt};
    case SyzygyWdl::blessed_loss:
        return {-1, std::nullopt};
    case SyzygyWdl::draw:
        return {0, std::nullopt};
    }
    return {};
}

SyzygyWdl syzygy_wdl_from_rank(const int rank) noexcept {
    if (rank >= 1'000) return SyzygyWdl::win;
    if (rank > 0) return SyzygyWdl::cursed_win;
    if (rank <= -1'000) return SyzygyWdl::loss;
    if (rank < 0) return SyzygyWdl::blessed_loss;
    return SyzygyWdl::draw;
}

SyzygyTablebase::SyzygyTablebase(std::filesystem::path path, const std::uint8_t probe_limit,
                                 const std::uint8_t probe_depth, const bool fifty_move_rule)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    // The vendored Fathom snapshot supports seven-man tables.  Keep the
    // default at five for predictable I/O, while allowing an explicit user
    // opt-in to six- and seven-piece probing.
    impl_->probe_limit = std::clamp<std::uint8_t>(probe_limit, 0, 7);
    impl_->probe_depth = std::clamp<std::uint8_t>(probe_depth, 1, 100);
    impl_->fifty_move_rule = fifty_move_rule;
    std::error_code path_error;
    const bool readable_directory = std::filesystem::is_directory(impl_->path, path_error);
    if (impl_->path.empty() || impl_->probe_limit == 0 || path_error || !readable_directory) {
        return;
    }
    impl_->enabled = acquire_fathom(impl_->path);
}

SyzygyTablebase::~SyzygyTablebase() {
    if (impl_->enabled) {
        release_fathom();
    }
}

bool SyzygyTablebase::enabled() const noexcept { return impl_->enabled; }

bool SyzygyTablebase::supports(const TablebaseSnapshot& snapshot) const noexcept {
    return valid_snapshot(snapshot) && snapshot.castling_rights == 0 &&
        snapshot.piece_count() <= impl_->probe_limit &&
        snapshot.piece_count() <= 7;
}

bool SyzygyTablebase::allows_depth(const int depth) const noexcept {
    return depth >= 1 && depth <= impl_->probe_depth;
}

std::uint8_t SyzygyTablebase::probe_limit() const noexcept { return impl_->probe_limit; }
std::uint8_t SyzygyTablebase::probe_depth() const noexcept { return impl_->probe_depth; }
bool SyzygyTablebase::fifty_move_rule() const noexcept { return impl_->fifty_move_rule; }
bool SyzygyTablebase::uses_clock_aware_root_probe() const noexcept {
    return impl_->fifty_move_rule;
}

std::optional<SyzygyWdl> SyzygyTablebase::probe_wdl(
    const TablebaseSnapshot& snapshot) const noexcept {
    if (!enabled() || !supports(snapshot) || (impl_->fifty_move_rule && snapshot.halfmove_clock != 0)) {
        return std::nullopt;
    }
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    native_bitboards(snapshot, white, black, kings, queens, rooks, bishops, knights, pawns);
    std::lock_guard lock(fathom_mutex());
    const unsigned result = tb_probe_wdl(white, black, kings, queens, rooks, bishops,
                                         knights, pawns, 0, snapshot.castling_rights,
                                         native_ep(snapshot), snapshot.side_to_move == Color::white);
    if (result == TB_RESULT_FAILED) {
        return std::nullopt;
    }
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    switch (TB_GET_WDL(result)) {
    case TB_LOSS: return SyzygyWdl::loss;
    case TB_BLESSED_LOSS: return SyzygyWdl::blessed_loss;
    case TB_DRAW: return SyzygyWdl::draw;
    case TB_CURSED_WIN: return SyzygyWdl::cursed_win;
    case TB_WIN: return SyzygyWdl::win;
    default: return std::nullopt;
    }
}

std::optional<SyzygyRootResult> SyzygyTablebase::probe_root(
    const GameState& state, const std::vector<Move>& allowed_moves) const noexcept {
    const TablebaseSnapshot snapshot = state.tablebase_snapshot();
    if (!enabled() || !supports(snapshot)) {
        return std::nullopt;
    }
    std::uint64_t white, black, kings, queens, rooks, bishops, knights, pawns;
    native_bitboards(snapshot, white, black, kings, queens, rooks, bishops, knights, pawns);
    TbRootMoves root_moves{};
    int probe_succeeded = 0;
    {
        std::lock_guard lock(fathom_mutex());
        if (impl_->fifty_move_rule) {
            probe_succeeded = tb_probe_root_dtz(
                white, black, kings, queens, rooks, bishops, knights, pawns,
                snapshot.halfmove_clock, snapshot.castling_rights,
                native_ep(snapshot), snapshot.side_to_move == Color::white,
                state.is_repetition_sensitive(), true, &root_moves);
            if (probe_succeeded == 0) {
                root_moves = {};
                probe_succeeded = tb_probe_root_wdl(
                    white, black, kings, queens, rooks, bishops, knights, pawns,
                    snapshot.halfmove_clock, snapshot.castling_rights,
                    native_ep(snapshot), snapshot.side_to_move == Color::white,
                    true, &root_moves);
            }
        } else {
            probe_succeeded = tb_probe_root_wdl(
                white, black, kings, queens, rooks, bishops, knights, pawns,
                snapshot.halfmove_clock, snapshot.castling_rights,
                native_ep(snapshot), snapshot.side_to_move == Color::white,
                false, &root_moves);
        }
    }
    if (probe_succeeded == 0 || root_moves.size == 0 || root_moves.size > TB_MAX_MOVES) {
        return std::nullopt;
    }
    const auto permitted = [&allowed_moves](const Move& move) {
        return allowed_moves.empty() || std::find(allowed_moves.begin(), allowed_moves.end(), move) !=
            allowed_moves.end();
    };
    int best_rank = std::numeric_limits<int>::min();
    std::vector<Move> best_moves;
    for (unsigned index = 0; index < root_moves.size; ++index) {
        const auto move = move_from_fathom(root_moves.moves[index].move);
        if (!move.has_value() || !permitted(*move) || !state.is_legal(*move)) {
            continue;
        }
        const int rank = root_moves.moves[index].tbRank;
        if (rank > best_rank) {
            best_rank = rank;
            best_moves.clear();
        }
        if (rank == best_rank) {
            best_moves.push_back(*move);
        }
    }
    if (best_moves.empty()) {
        return std::nullopt;
    }
    const SyzygyWdl wdl = syzygy_wdl_from_rank(best_rank);
    SyzygyRootResult result{wdl, syzygy_score(wdl), std::move(best_moves)};
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::uint64_t SyzygyTablebase::hits() const noexcept {
    return impl_->hits.load(std::memory_order_relaxed);
}

} // namespace koi
