#pragma once

#include <memory>

#include "koi/evaluator.hpp"
#include "koi/search_types.hpp"

namespace koi {

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

private:
    struct State;

    explicit SearchHandle(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> state_;

    friend class SearchService;
};

class SearchService {
public:
    explicit SearchService(std::shared_ptr<const Evaluator> evaluator);

    [[nodiscard]] SearchHandle start(GameState root, SearchLimits limits,
                                     SearchEventSink sink = {}, SearchOptions options = {});
    void set_hash_size_mb(std::size_t megabytes);
    [[nodiscard]] std::size_t hash_size_mb() const noexcept;
    void clear_hash() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace koi
