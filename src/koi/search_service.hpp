#pragma once

#include <memory>

#include "koi/evaluator.hpp"
#include "koi/detail/search_constants.hpp"
#include "koi/search_types.hpp"
#include "koi/transposition_table.hpp"

namespace koi {

namespace detail {

class SearchSession;

} // namespace detail

class SearchHandle {
public:
    SearchHandle(SearchHandle&&) noexcept;
    SearchHandle& operator=(SearchHandle&&) noexcept;
    SearchHandle(const SearchHandle&) = delete;
    SearchHandle& operator=(const SearchHandle&) = delete;
    ~SearchHandle();

    void stop() noexcept;
    void wait();
    [[nodiscard]] bool running() const noexcept;
    void request_ponderhit(SearchLimits limits);

private:
    explicit SearchHandle(std::shared_ptr<detail::SearchSession> state) noexcept;

    std::shared_ptr<detail::SearchSession> state_;

    friend class SearchService;
};

class SearchService {
public:
    explicit SearchService(std::shared_ptr<const Evaluator> evaluator,
                           HashMemoryPolicy hash_memory_policy = {},
                           std::size_t initial_hash_mb = 512);

    [[nodiscard]] SearchHandle start(GameState root, SearchLimits limits,
                                     SearchEventSink sink = {}, SearchOptions options = {});
    [[nodiscard]] HashResizeResult set_hash_size_mb(std::size_t megabytes);
    // Replaces the evaluator used by subsequent searches.  A search that is
    // already running keeps the evaluator it started with; the next start()
    // observes the new one.  The caller must not pass a null pointer.
    void set_evaluator(std::shared_ptr<const Evaluator> evaluator);
    [[nodiscard]] std::size_t hash_size_mb() const noexcept;
    // Approximate transposition-table occupancy in permill (0..1000) for UCI
    // `info ... hashfull` reporting.
    [[nodiscard]] std::size_t hashfull_permill() const noexcept;
    void clear_hash() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace koi
