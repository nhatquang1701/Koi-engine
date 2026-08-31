#include "koi/move.hpp"

namespace koi {

std::optional<Square> Square::parse(std::string_view coordinate) noexcept {
    if (coordinate.size() != 2 || coordinate[0] < 'a' || coordinate[0] > 'h' || coordinate[1] < '1' ||
        coordinate[1] > '8') {
        return std::nullopt;
    }

    return Square::from_index(static_cast<std::uint8_t>((coordinate[1] - '1') * 8 + coordinate[0] - 'a'));
}

std::string Square::uci() const {
    if (index() == kInvalid) {
        return {};
    }

    return {file(), rank()};
}

Move Move::no_move() noexcept {
    return {};
}

std::optional<Move> Move::parse_uci(std::string_view uci) noexcept {
    if (uci == "0000") {
        return Move::no_move();
    }
    if (uci.size() != 4 && uci.size() != 5) {
        return std::nullopt;
    }

    const auto from = Square::parse(uci.substr(0, 2));
    const auto to = Square::parse(uci.substr(2, 2));
    if (!from || !to) {
        return std::nullopt;
    }

    Promotion promotion = Promotion::none;
    if (uci.size() == 5) {
        switch (uci[4]) {
            case 'n':
                promotion = Promotion::knight;
                break;
            case 'b':
                promotion = Promotion::bishop;
                break;
            case 'r':
                promotion = Promotion::rook;
                break;
            case 'q':
                promotion = Promotion::queen;
                break;
            default:
                return std::nullopt;
        }
    }

    return Move(*from, *to, promotion);
}

std::string Move::uci() const {
    if (is_no_move()) {
        return "0000";
    }
    if (from().index() == Square::kInvalid || to().index() == Square::kInvalid) {
        return {};
    }

    std::string result = from().uci() + to().uci();
    switch (promotion()) {
        case Promotion::none:
            break;
        case Promotion::knight:
            result += 'n';
            break;
        case Promotion::bishop:
            result += 'b';
            break;
        case Promotion::rook:
            result += 'r';
            break;
        case Promotion::queen:
            result += 'q';
            break;
    }
    return result;
}

} // namespace koi
