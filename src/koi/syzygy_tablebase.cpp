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

SyzygyWdl wdl_from_rank(int rank) noexcept {
    return rank > 0 ? SyzygyWdl::win : rank < 0 ? SyzygyWdl::loss : SyzygyWdl::draw;
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
    case SyzygyWdl::cursed_win:
        return {100'000 - 1, 1};
    case SyzygyWdl::loss:
    case SyzygyWdl::blessed_loss:
        return {-100'000 + 1, -1};
    case SyzygyWdl::draw:
        return {0, std::nullopt};
    }
    return {};
}

SyzygyTablebase::SyzygyTablebase(std::filesystem::path path, const std::uint8_t probe_limit,
                                 const std::uint8_t probe_depth, const bool fifty_move_rule)
    : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path);
    impl_->probe_limit = std::clamp<std::uint8_t>(probe_limit, 0, 5);
    impl_->probe_depth = std::clamp<std::uint8_t>(probe_depth, 1, 100);
    impl_->fifty_move_rule = fifty_move_rule;
    std::error_code path_error;
    const bool readable_directory = std::filesystem::is_directory(impl_->path, path_error);
    if (impl_->path.empty() || impl_->probe_limit == 0 || path_error || !readable_directory) {
        return;
    }
    std::lock_guard lock(fathom_mutex());
    impl_->enabled = tb_init(impl_->path.string().c_str()) && TB_LARGEST > 0;
}

SyzygyTablebase::~SyzygyTablebase() {
    if (impl_->enabled) {
        std::lock_guard lock(fathom_mutex());
        tb_free();
    }
}

bool SyzygyTablebase::enabled() const noexcept { return impl_->enabled; }

bool SyzygyTablebase::supports(const TablebaseSnapshot& snapshot) const noexcept {
    return snapshot.castling_rights == 0 && snapshot.piece_count() <= impl_->probe_limit &&
        snapshot.piece_count() <= 5;
}

bool SyzygyTablebase::allows_depth(const int depth) const noexcept {
    return depth >= 1 && depth <= impl_->probe_depth;
}

std::uint8_t SyzygyTablebase::probe_limit() const noexcept { return impl_->probe_limit; }
std::uint8_t SyzygyTablebase::probe_depth() const noexcept { return impl_->probe_depth; }
bool SyzygyTablebase::fifty_move_rule() const noexcept { return impl_->fifty_move_rule; }

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
    std::lock_guard lock(fathom_mutex());
    if (tb_probe_root_wdl(white, black, kings, queens, rooks, bishops, knights, pawns,
                          snapshot.halfmove_clock, snapshot.castling_rights,
                          native_ep(snapshot), snapshot.side_to_move == Color::white,
                          impl_->fifty_move_rule, &root_moves) == 0 || root_moves.size == 0) {
        return std::nullopt;
    }
    int best_rank = std::numeric_limits<int>::min();
    for (unsigned index = 0; index < root_moves.size; ++index) {
        best_rank = std::max(best_rank, root_moves.moves[index].tbRank);
    }
    const SyzygyWdl wdl = wdl_from_rank(best_rank);
    SyzygyRootResult result{wdl, syzygy_score(wdl), {}};
    const auto permitted = [&allowed_moves](const Move& move) {
        return allowed_moves.empty() || std::find(allowed_moves.begin(), allowed_moves.end(), move) !=
            allowed_moves.end();
    };
    for (unsigned index = 0; index < root_moves.size; ++index) {
        if (root_moves.moves[index].tbRank != best_rank) continue;
        const auto move = move_from_fathom(root_moves.moves[index].move);
        if (move.has_value() && permitted(*move) && state.is_legal(*move)) {
            result.moves.push_back(*move);
        }
    }
    if (result.moves.empty()) {
        return std::nullopt;
    }
    impl_->hits.fetch_add(1, std::memory_order_relaxed);
    return result;
}

std::uint64_t SyzygyTablebase::hits() const noexcept {
    return impl_->hits.load(std::memory_order_relaxed);
}

} // namespace koi
