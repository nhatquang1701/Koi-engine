#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "koi/game_state.hpp"

namespace koi {

enum class SyzygyWdl : std::int8_t {
    loss = -2,
    blessed_loss = -1,
    draw = 0,
    cursed_win = 1,
    win = 2,
};

struct SyzygyScore {
    int score_cp = 0;
    std::optional<int> mate;
};

[[nodiscard]] SyzygyScore syzygy_score(SyzygyWdl wdl) noexcept;
[[nodiscard]] SyzygyWdl syzygy_wdl_from_rank(int rank) noexcept;

struct SyzygyRootResult {
    SyzygyWdl wdl = SyzygyWdl::draw;
    SyzygyScore score{};
    std::vector<Move> moves;
};

class SyzygyTablebase {
public:
    // Fathom is process-global; instances share one acquired path and release it
    // only after the final enabled instance is destroyed.
    SyzygyTablebase(std::filesystem::path path = {}, std::uint8_t probe_limit = 5,
                    std::uint8_t probe_depth = 1, bool fifty_move_rule = true);
    ~SyzygyTablebase();

    SyzygyTablebase(const SyzygyTablebase&) = delete;
    SyzygyTablebase& operator=(const SyzygyTablebase&) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    // Largest table registered in the loaded directory (0 when disabled).
    // While this instance is enabled Fathom cannot be re-initialised, so the
    // underlying TB_LARGEST global is stable and safe to read.
    [[nodiscard]] int large_table_limit() const noexcept;
    // Cheap pre-gate for tree probes.  It only consults the native piece count
    // and castling rights, so callers can skip building a TablebaseSnapshot and
    // running the full bitboard validation for positions that cannot be probed.
    // A true result is permissive: probe/root probing still validates fully.
    [[nodiscard]] bool probe_eligible(const GameState& state) const noexcept;
    [[nodiscard]] bool supports(const TablebaseSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool allows_depth(int depth) const noexcept;
    [[nodiscard]] std::uint8_t probe_limit() const noexcept;
    [[nodiscard]] std::uint8_t probe_depth() const noexcept;
    [[nodiscard]] bool fifty_move_rule() const noexcept;
    [[nodiscard]] bool uses_clock_aware_root_probe() const noexcept;
    // Fathom documents tb_probe_wdl as thread safe, so no adapter lock is taken
    // here.  Root probing uses tb_probe_root, which is documented NOT thread
    // safe and therefore stays serialised inside this adapter.
    [[nodiscard]] std::optional<SyzygyWdl> probe_wdl(const TablebaseSnapshot& snapshot) const noexcept;
    [[nodiscard]] std::optional<SyzygyRootResult> probe_root(
        const GameState& state, const std::vector<Move>& allowed_moves = {}) const noexcept;
    [[nodiscard]] std::uint64_t hits() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
