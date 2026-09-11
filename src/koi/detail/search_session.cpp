#include "koi/detail/search_session.hpp"

#include <stdexcept>
#include <utility>

namespace koi::detail {

SearchSession::SearchSession(GameState root, SearchLimits limits, SearchOptions options)
    : root_(std::move(root)), limits_(std::move(limits)), options_(std::move(options)),
      identity_{options_.generation, root_.position_key(), root_.fen()} {}

SearchSession::~SearchSession() {
    stop();
    wait();
}

void SearchSession::launch(std::function<void()> work) {
    if (!work || worker_.joinable()) {
        throw std::logic_error("search session can only launch one worker");
    }
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this, work = std::move(work)]() mutable {
            try {
                work();
            } catch (...) {
                // SearchService owns result construction and its worker body
                // already converts failures into a failed SearchResult. This
                // guard keeps the lifecycle state correct for other private
                // session users as well.
            }
            mark_finished();
        });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        throw;
    }
}

void SearchSession::stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
    stop_condition_.notify_all();
}

void SearchSession::wait() {
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
}

void SearchSession::wait_until_stopped() {
    std::unique_lock lock(stop_mutex_);
    stop_condition_.wait(lock, [this] {
        return stop_requested_.load(std::memory_order_relaxed);
    });
}

bool SearchSession::publish_completion(const SearchEventSink& sink,
                                       const SearchResult& result) noexcept {
    if (!completion_once_.try_claim()) {
        return false;
    }
    running_.store(false, std::memory_order_release);
    stop_condition_.notify_all();
    if (sink.on_complete) {
        try {
            sink.on_complete(result);
        } catch (...) {
        }
    }
    return true;
}

void SearchSession::mark_finished() noexcept {
    running_.store(false, std::memory_order_release);
    stop_condition_.notify_all();
}

} // namespace koi::detail
