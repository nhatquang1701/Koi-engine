#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "koi/completion_gate.hpp"

namespace koi::detail {

// Owns the lifecycle of exactly one search request. The request snapshots are
// immutable after construction; the working search context is owned by the
// worker launched for this session.
class SearchSession {
public:
    SearchSession(GameState root, SearchLimits limits, SearchOptions options);
    SearchSession(const SearchSession&) = delete;
    SearchSession& operator=(const SearchSession&) = delete;
    ~SearchSession();

    [[nodiscard]] const GameState& root() const noexcept { return root_; }
    [[nodiscard]] const SearchLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const SearchOptions& options() const noexcept { return options_; }
    [[nodiscard]] const SearchRequestIdentity& identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return identity_.generation; }
    [[nodiscard]] std::atomic_bool& stop_requested() noexcept { return stop_requested_; }

    void launch(std::function<void()> work);
    void stop() noexcept;
    void wait();
    void wait_until_stopped();
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool publish_completion(const SearchEventSink& sink,
                                          const SearchResult& result) noexcept;

private:
    void mark_finished() noexcept;

    GameState root_;
    SearchLimits limits_;
    SearchOptions options_;
    SearchRequestIdentity identity_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_bool running_ = false;
    CompletionOnce completion_once_;
    std::mutex stop_mutex_;
    std::condition_variable stop_condition_;
    std::thread worker_;
};

} // namespace koi::detail
