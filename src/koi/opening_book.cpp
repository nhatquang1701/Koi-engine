#include "koi/opening_book.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <unordered_map>
#include <utility>
#include <vector>

#include "koi/game_state.hpp"

namespace koi {

namespace {

struct Record {
    std::uint16_t move = 0;
    std::uint16_t weight = 0;
    std::uint32_t learn = 0;
};

struct FileSignature {
    std::filesystem::path path;
    std::filesystem::file_time_type timestamp{};
    bool exists = false;

    friend bool operator==(const FileSignature&, const FileSignature&) = default;
};

template <class UInt>
UInt read_big_endian(const std::array<unsigned char, 16>& bytes, std::size_t offset) noexcept {
    UInt value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value = static_cast<UInt>((value << 8) | bytes[offset + index]);
    }
    return value;
}

std::optional<FileSignature> signature_for(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        return std::nullopt;
    }
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        return std::nullopt;
    }
    const bool exists = std::filesystem::is_regular_file(canonical, error);
    if (error) {
        return std::nullopt;
    }
    if (!exists) {
        return FileSignature{canonical, {}, false};
    }
    const std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(canonical, error);
    if (error) {
        return std::nullopt;
    }
    return FileSignature{canonical, timestamp, true};
}

std::optional<Move> decode_move(std::uint16_t encoded, const GameState& state) {
    const std::uint8_t source = static_cast<std::uint8_t>(encoded & 0x3f);
    std::uint8_t target = static_cast<std::uint8_t>((encoded >> 6) & 0x3f);
    const std::uint8_t promotion_code = static_cast<std::uint8_t>((encoded >> 12) & 0x7);
    Promotion promotion = Promotion::none;
    switch (promotion_code) {
    case 0:
        break;
    case 1:
        promotion = Promotion::knight;
        break;
    case 2:
        promotion = Promotion::bishop;
        break;
    case 3:
        promotion = Promotion::rook;
        break;
    case 4:
        promotion = Promotion::queen;
        break;
    default:
        return std::nullopt;
    }

    const Square from = Square::from_index(source);
    const Square encoded_to = Square::from_index(target);
    const Piece moving_piece = state.piece_at(from);
    const Piece destination_piece = state.piece_at(encoded_to);
    if (moving_piece.type == PieceType::king && destination_piece.type == PieceType::rook &&
        moving_piece.color == destination_piece.color && from.rank() == encoded_to.rank()) {
        target = static_cast<std::uint8_t>(from.index() / 8 * 8 + (encoded_to.index() > from.index() ? 6 : 2));
    }

    const Move move(from, Square::from_index(target), promotion);
    return state.is_legal(move) ? std::optional<Move>(move) : std::nullopt;
}

std::uint32_t book_move_tie_break_key(Move move) noexcept {
    const auto square_key = [](Square square) noexcept -> std::uint32_t {
        if (square.index() == Square::kInvalid) {
            return 64;
        }
        return static_cast<std::uint32_t>(square.index() % 8) * 8U + square.index() / 8U;
    };

    std::uint32_t promotion_key = 0;
    switch (move.promotion()) {
    case Promotion::bishop:
        promotion_key = 1;
        break;
    case Promotion::knight:
        promotion_key = 2;
        break;
    case Promotion::queen:
        promotion_key = 3;
        break;
    case Promotion::rook:
        promotion_key = 4;
        break;
    case Promotion::none:
        break;
    }

    return (square_key(move.from()) * 64U + square_key(move.to())) * 5U + promotion_key;
}

constexpr int kBookSafetyLossThreshold = 250;

int piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn: return 100;
    case PieceType::knight: return 320;
    case PieceType::bishop: return 330;
    case PieceType::rook: return 500;
    case PieceType::queen: return 900;
    case PieceType::king: return 20'000;
    case PieceType::none: break;
    }
    return 0;
}

int material_score(const GameState& state, Color perspective) noexcept {
    int score = 0;
    for (const Piece& piece : state.position_features().board) {
        if (piece.type == PieceType::none) {
            continue;
        }
        const int signed_value = piece_value(piece.type) *
            (piece.color == perspective ? 1 : -1);
        score += signed_value;
    }
    return score;
}

int forcing_material_probe(GameState& state, Color perspective, int depth,
                           bool maximizing) noexcept {
    if (depth <= 0) {
        return material_score(state, perspective);
    }

    MoveMetadataList moves;
    const bool has_legal_move = state.legal_tactical_moves_with_metadata(moves, true);
    if (!has_legal_move || moves.empty()) {
        return material_score(state, perspective);
    }

    int best = maximizing ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
    for (const MoveMetadata& metadata : moves) {
        GameState child = state;
        if (!child.make_legal_move(metadata)) {
            continue;
        }
        const int score = forcing_material_probe(child, perspective, depth - 1, !maximizing);
        best = maximizing ? std::max(best, score) : std::min(best, score);
    }
    return best == std::numeric_limits<int>::min() || best == std::numeric_limits<int>::max() ?
        material_score(state, perspective) : best;
}

bool safe_book_move(const GameState& state, const Move& move, std::uint8_t safety_depth) noexcept {
    if (safety_depth == 0) {
        return true;
    }
    const auto metadata = state.describe_move(move);
    if (!metadata.has_value()) {
        return false;
    }
    GameState after = state;
    if (!after.make_legal_move(*metadata)) {
        return false;
    }
    if (after.is_terminal()) {
        return true;
    }

    const Color perspective = state.side_to_move();
    const int before = material_score(state, perspective);
    const int probe_depth = std::min<int>(safety_depth, 3);
    const int worst_forcing_line = forcing_material_probe(
        after, perspective, probe_depth, false);
    return worst_forcing_line >= before - kBookSafetyLossThreshold;
}

} // namespace

class OpeningBook::Impl {
public:
    struct Cache {
        FileSignature signature;
        std::unordered_map<std::uint64_t, std::vector<Record>> records;
        bool usable = false;
    };

    explicit Impl(std::filesystem::path directory)
        : executable_directory(std::move(directory)), file("book.bin") {}

    [[nodiscard]] std::shared_ptr<const Cache> load() const {
        std::scoped_lock lock(mutex);
        std::filesystem::path resolved = file;
        if (resolved.is_relative() && !executable_directory.empty()) {
            resolved = executable_directory / resolved;
        }
        const std::optional<FileSignature> signature = signature_for(resolved);
        if (!signature) {
            return {};
        }
        if (cache && cache->signature == *signature) {
            return cache;
        }

        auto replacement = std::make_shared<Cache>();
        replacement->signature = *signature;
        if (!signature->exists) {
            cache = replacement;
            return cache;
        }

        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(signature->path, error);
        if (error || size == 0 || size % 16 != 0) {
            cache = replacement;
            return cache;
        }

        std::ifstream stream(signature->path, std::ios::binary);
        if (!stream) {
            cache = replacement;
            return cache;
        }
        for (std::uintmax_t offset = 0; offset < size; offset += 16) {
            std::array<unsigned char, 16> bytes{};
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream) {
                cache = replacement;
                return cache;
            }
            const std::uint64_t key = read_big_endian<std::uint64_t>(bytes, 0);
            replacement->records[key].push_back(Record{
                read_big_endian<std::uint16_t>(bytes, 8),
                read_big_endian<std::uint16_t>(bytes, 10),
                read_big_endian<std::uint32_t>(bytes, 12),
            });
        }
        replacement->usable = !replacement->records.empty();
        cache = replacement;
        return cache;
    }

    void set_file(std::filesystem::path path) {
        std::scoped_lock lock(mutex);
        file = std::move(path);
        cache.reset();
    }

    void clear_cache() noexcept {
        try {
            std::scoped_lock lock(mutex);
            cache.reset();
        } catch (...) {
        }
    }

    std::filesystem::path executable_directory;
    std::filesystem::path file;
    mutable std::mutex mutex;
    mutable std::shared_ptr<const Cache> cache;
};

OpeningBook::OpeningBook(std::filesystem::path executable_directory)
    : impl_(std::make_shared<Impl>(std::move(executable_directory))) {}

void OpeningBook::set_file(std::filesystem::path path) {
    impl_->set_file(std::move(path));
}

void OpeningBook::clear_cache() noexcept {
    impl_->clear_cache();
}

std::optional<BookChoice> OpeningBook::choose(
    const GameState& state, std::uint32_t root_ply, bool enabled,
    std::uint8_t maximum_depth, std::uint64_t random_seed, bool random_selection,
    bool safety_enabled, std::uint8_t safety_depth) const {
    if (!enabled || (maximum_depth != 0 && root_ply >= maximum_depth)) {
        return std::nullopt;
    }

    const std::uint64_t key = state.polyglot_key();
    const std::shared_ptr<const Impl::Cache> cache = impl_->load();
    if (!cache || !cache->usable) {
        return std::nullopt;
    }
    const auto entries = cache->records.find(key);
    if (entries == cache->records.end()) {
        return std::nullopt;
    }

    std::vector<BookChoice> choices;
    std::uint64_t total_weight = 0;
    for (const Record& entry : entries->second) {
        if (entry.weight == 0) {
            continue;
        }
        const std::optional<Move> move = decode_move(entry.move, state);
        if (!move || total_weight > std::numeric_limits<std::uint64_t>::max() - entry.weight) {
            continue;
        }
        if (safety_enabled && !safe_book_move(state, *move, safety_depth)) {
            continue;
        }
        choices.push_back(BookChoice{*move, entry.weight, entry.learn});
        total_weight += entry.weight;
    }
    if (choices.empty() || total_weight == 0) {
        return std::nullopt;
    }

    if (!random_selection) {
        const auto best = std::max_element(
            choices.begin(), choices.end(), [](const BookChoice& left, const BookChoice& right) {
                if (left.weight != right.weight) {
                    return left.weight < right.weight;
                }
                return book_move_tie_break_key(left.move) > book_move_tie_break_key(right.move);
            });
        return best == choices.end() ? std::nullopt : std::optional<BookChoice>{*best};
    }

    std::uint64_t seed = random_seed ^ key;
    if (random_seed == 0) {
        std::random_device entropy;
        seed = (static_cast<std::uint64_t>(entropy()) << 32) ^ entropy() ^
            static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    }
    std::mt19937_64 generator(seed);
    std::uniform_int_distribution<std::uint64_t> distribution(0, total_weight - 1);
    std::uint64_t selected = distribution(generator);
    for (const BookChoice& choice : choices) {
        if (selected < choice.weight) {
            return choice;
        }
        selected -= choice.weight;
    }
    return std::nullopt;
}

} // namespace koi
