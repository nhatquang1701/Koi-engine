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

struct SyzygyRootResult {
    SyzygyWdl wdl = SyzygyWdl::draw;
    SyzygyScore score{};
    std::vector<Move> moves;
};

class SyzygyTablebase {
public:
    SyzygyTablebase(std::filesystem::path path = {}, std::uint8_t probe_limit = 5,
                    std::uint8_t probe_depth = 1, bool fifty_move_rule = true);
    ~SyzygyTablebase();

    SyzygyTablebase(const SyzygyTablebase&) = delete;
    SyzygyTablebase& operator=(const SyzygyTablebase&) = delete;

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool supports(const TablebaseSnapshot& snapshot) const noexcept;
    [[nodiscard]] bool allows_depth(int depth) const noexcept;
    [[nodiscard]] std::uint8_t probe_limit() const noexcept;
    [[nodiscard]] std::uint8_t probe_depth() const noexcept;
    [[nodiscard]] bool fifty_move_rule() const noexcept;
    [[nodiscard]] std::optional<SyzygyWdl> probe_wdl(const TablebaseSnapshot& snapshot) const noexcept;
    [[nodiscard]] std::optional<SyzygyRootResult> probe_root(
        const GameState& state, const std::vector<Move>& allowed_moves = {}) const noexcept;
    [[nodiscard]] std::uint64_t hits() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace koi
