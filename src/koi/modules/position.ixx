module;
#include <cstdint>

export module koi:position;

export import :types;

export namespace koi::module_api {
inline constexpr unsigned position_boundary_version = 1;

struct PositionContract {
    std::uint64_t zobrist_key = 0;
    std::uint8_t side_to_move = 0;
    std::uint8_t piece_count = 0;
    std::uint8_t castling_rights = 0;
    std::uint8_t en_passant_square = 64;
    std::uint16_t halfmove_clock = 0;
    std::uint16_t fullmove_number = 1;
    bool in_check = false;
    bool terminal = false;
};
}
