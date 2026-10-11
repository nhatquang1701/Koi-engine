#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>

namespace koi::detail {

// Fixed-capacity storage whose elements are never constructed.  Element
// lifetimes start on demand through std::start_lifetime_as_array (C++23), so
// an untouched slot costs no stores at all.  A plain std::array<Move, N>
// member would otherwise store Move's no-move sentinel into every element
// whenever the enclosing per-node container is constructed, even though all
// readers are bounded by a live size or length.
//
// Requires T to be an implicit-lifetime, trivially copyable type (Move and
// MoveMetadata qualify).  The contract mirrors MoveMetadataList: only
// elements in [0, n) are ever read, and a caller writes an element before
// reading it.  No wholesale copies are provided because the untouched tail
// has no meaningful value; owners copy their live prefix explicitly.
template <typename T, std::size_t N>
class NoInitArray {
    static_assert(std::is_trivially_copyable_v<T>,
                  "NoInitArray elements must be trivially copyable");

public:
    NoInitArray() noexcept = default;
    NoInitArray(const NoInitArray&) = delete;
    NoInitArray& operator=(const NoInitArray&) = delete;
    NoInitArray(NoInitArray&&) = delete;
    NoInitArray& operator=(NoInitArray&&) = delete;
    ~NoInitArray() = default;

    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }

    [[nodiscard]] T* data() noexcept {
#if defined(__cpp_lib_start_lifetime_as)
        return std::start_lifetime_as_array<T>(bytes_, N);
#else
        // Toolchains without P2590R2 (MSVC as of 14.44) keep the practical
        // pattern: the storage is correctly aligned and T is an
        // implicit-lifetime type whose elements are always written before
        // they are read, so the cast is the only spelling available here.
        return reinterpret_cast<T*>(bytes_);
#endif
    }

    [[nodiscard]] const T* data() const noexcept {
#if defined(__cpp_lib_start_lifetime_as)
        // std::start_lifetime_as_array (re)creates the element objects, so it
        // takes a non-const pointer; the const overload maps to the same
        // storage through the object that owns it.
        return std::start_lifetime_as_array<T>(const_cast<std::byte*>(bytes_), N);
#else
        return reinterpret_cast<const T*>(bytes_);
#endif
    }

    [[nodiscard]] T& operator[](std::size_t index) noexcept { return data()[index]; }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept {
        return data()[index];
    }

private:
    alignas(T) std::byte bytes_[sizeof(T) * N];
};

} // namespace koi::detail
