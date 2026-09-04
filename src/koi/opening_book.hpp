#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include "koi/move.hpp"

namespace koi {

class GameState;

struct BookChoice {
    Move move;
    std::uint16_t weight = 0;
    std::uint32_t learn = 0;
};

class OpeningBook {
public:
    explicit OpeningBook(std::filesystem::path executable_directory = {});
    void set_file(std::filesystem::path path);
    void clear_cache() noexcept;
    [[nodiscard]] std::optional<BookChoice> choose(
        const GameState& state, std::uint32_t root_ply, bool enabled,
        std::uint8_t maximum_depth, std::uint64_t random_seed) const;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace koi
