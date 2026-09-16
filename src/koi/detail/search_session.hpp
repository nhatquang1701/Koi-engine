#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
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
    void wait_until_stopped_or_ponderhit();
    [[nodiscard]] bool running() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool publish_completion(const SearchEventSink& sink,
                                          const SearchResult& result) noexcept;

    // Records a UCI ponderhit conversion for the running worker. The worker
    // consumes it at the top of the next iteration and re-arms timing.
    void request_ponderhit(SearchLimits limits);
    [[nodiscard]] bool ponderhit_requested() const noexcept {
        return ponderhit_requested_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<SearchLimits> take_ponderhit_limits();

private:
    void mark_finished() noexcept;

    GameState root_;
    SearchLimits limits_;
    SearchOptions options_;
    SearchRequestIdentity identity_;
    std::atomic_bool stop_requested_ = false;
    std::atomic_bool running_ = false;
    std::atomic_bool ponderhit_requested_ = false;
    CompletionOnce completion_once_;
    std::mutex stop_mutex_;
    std::condition_variable stop_condition_;
    std::mutex ponderhit_mutex_;
    std::optional<SearchLimits> ponderhit_limits_;
    std::thread worker_;
};

} // namespace koi::detail
