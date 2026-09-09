#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace koi {

enum class Color : std::uint8_t { white, black };

[[nodiscard]] constexpr Color opposite(Color color) noexcept {
    return color == Color::white ? Color::black : Color::white;
}

enum class PieceType : std::uint8_t { none, pawn, knight, bishop, rook, queen, king };

struct Piece {
    PieceType type = PieceType::none;
    Color color = Color::white;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return type == PieceType::none;
    }
};

struct Square {
    static constexpr std::uint8_t kInvalid = 64;

    constexpr Square() noexcept = default;

    [[nodiscard]] static constexpr Square from_index(std::uint8_t index) noexcept {
        Square square;
        square.index_ = index < kInvalid ? index : kInvalid;
        return square;
    }

    [[nodiscard]] static std::optional<Square> parse(std::string_view coordinate) noexcept;

    [[nodiscard]] constexpr std::uint8_t index() const noexcept {
        return index_;
    }

    [[nodiscard]] constexpr char file() const noexcept {
        return index_ < kInvalid ? static_cast<char>('a' + (index_ % 8)) : '\0';
    }

    [[nodiscard]] constexpr char rank() const noexcept {
        return index_ < kInvalid ? static_cast<char>('1' + (index_ / 8)) : '\0';
    }

    [[nodiscard]] std::string uci() const;

    friend constexpr bool operator==(Square, Square) noexcept = default;

private:
    std::uint8_t index_ = kInvalid;
};

enum class Promotion : std::uint8_t { none, knight, bishop, rook, queen };

class Move {
public:
    constexpr Move() noexcept = default;
    constexpr Move(Square from, Square to, Promotion promotion = Promotion::none) noexcept
        : packed_(encode(from, to, promotion)) {}

    [[nodiscard]] static Move no_move() noexcept;
    [[nodiscard]] static std::optional<Move> parse_uci(std::string_view uci) noexcept;
    [[nodiscard]] constexpr Square from() const noexcept;
    [[nodiscard]] constexpr Square to() const noexcept;
    [[nodiscard]] constexpr Promotion promotion() const noexcept;
    [[nodiscard]] constexpr bool is_no_move() const noexcept;
    [[nodiscard]] std::string uci() const;

    friend constexpr bool operator==(const Move&, const Move&) noexcept = default;

private:
    static constexpr std::uint32_t kNoMove = 0xFFFFFFFFU;

    [[nodiscard]] static constexpr std::uint32_t encode(
        Square from, Square to, Promotion promotion) noexcept {
        if (from.index() >= Square::kInvalid || to.index() >= Square::kInvalid) {
            return kNoMove;
        }
        return static_cast<std::uint32_t>(from.index()) |
            (static_cast<std::uint32_t>(to.index()) << 6U) |
            (static_cast<std::uint32_t>(promotion) << 12U);
    }

    std::uint32_t packed_ = kNoMove;
};

constexpr Square Move::from() const noexcept {
    return is_no_move() ? Square{} :
        Square::from_index(static_cast<std::uint8_t>(packed_ & 0x3FU));
}

constexpr Square Move::to() const noexcept {
    return is_no_move() ? Square{} :
        Square::from_index(static_cast<std::uint8_t>((packed_ >> 6U) & 0x3FU));
}

constexpr Promotion Move::promotion() const noexcept {
    return is_no_move() ? Promotion::none :
        static_cast<Promotion>((packed_ >> 12U) & 0x7U);
}

constexpr bool Move::is_no_move() const noexcept {
    return packed_ == kNoMove;
}

} // namespace koi
