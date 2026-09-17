#pragma once

// Shared Polyglot opening-book fixtures for the runtime test binaries.
//
// Before this header existed the `BookRecord` layout, the big-endian record
// serializer, and the Polyglot move encoder were copy-pasted into every test
// that writes a synthetic `.bin` book. Keep them here so the on-disk format is
// described exactly once.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "koi/move.hpp"

#include "koi_test_support.hpp"

namespace koi::test::book {

struct BookRecord {
    std::uint64_t key;
    std::uint16_t move;
    std::uint16_t weight;
    std::uint32_t learn;
};

template <class UInt>
void append_big_endian(std::vector<char>& bytes, UInt value) {
    for (int shift = static_cast<int>(sizeof(UInt) * 8) - 8; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

inline void write_book(const std::filesystem::path& path,
                       const std::vector<BookRecord>& records) {
    std::vector<char> bytes;
    bytes.reserve(records.size() * 16);
    for (const BookRecord& record : records) {
        append_big_endian(bytes, record.key);
        append_big_endian(bytes, record.move);
        append_big_endian(bytes, record.weight);
        append_big_endian(bytes, record.learn);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(stream.good(), "test book must be writable");
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(stream.good(), "test book must be completely written");
}

// Polyglot stores the destination in bits 0..5, the origin in bits 6..11, and
// the promotion piece in bits 12..14. The three-argument overload preserves the
// opening-book tests' promotion-capable signature; the two-argument overload
// preserves the UCI controller tests' non-promotion signature.
inline std::uint16_t polyglot_move(std::string_view from, std::string_view to,
                                   std::uint8_t promotion) {
    const auto source = koi::Square::parse(from);
    const auto target = koi::Square::parse(to);
    require(source.has_value() && target.has_value(),
            "Polyglot coordinate fixture must be valid");
    return static_cast<std::uint16_t>(target->index() | (source->index() << 6) |
                                      (promotion << 12));
}

inline std::uint16_t polyglot_move(std::string_view from, std::string_view to) {
    return polyglot_move(from, to, 0);
}

} // namespace koi::test::book
