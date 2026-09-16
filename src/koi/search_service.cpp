#include "koi/search_service.hpp"

#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "koi/detail/search_runner.hpp"
#include "koi/detail/search_session.hpp"

namespace koi {

struct SearchService::Impl {
    explicit Impl(std::shared_ptr<const Evaluator> evaluator, HashMemoryPolicy hash_memory_policy,
                  std::size_t initial_hash_mb)
        : evaluator(std::move(evaluator)),
          table(std::make_shared<TranspositionTable>(initial_hash_mb, std::move(hash_memory_policy))),
          evaluator_mutex(std::make_shared<std::mutex>()) {}

    std::shared_ptr<const Evaluator> evaluator;
    std::shared_ptr<TranspositionTable> table;
    std::shared_ptr<std::mutex> evaluator_mutex;
};

SearchHandle::SearchHandle(std::shared_ptr<detail::SearchSession> state) noexcept
    : state_(std::move(state)) {}

SearchHandle::SearchHandle(SearchHandle&&) noexcept = default;

SearchHandle& SearchHandle::operator=(SearchHandle&& other) noexcept {
    if (this != &other) {
        stop();
        wait();
        state_ = std::move(other.state_);
    }
    return *this;
}

SearchHandle::~SearchHandle() {
    stop();
    wait();
}

void SearchHandle::stop() noexcept {
    if (state_) {
        state_->stop();
    }
}

void SearchHandle::wait() {
    if (state_) {
        state_->wait();
    }
}

bool SearchHandle::running() const noexcept {
    return state_ && state_->running();
}

void SearchHandle::request_ponderhit(SearchLimits limits) {
    if (state_) {
        state_->request_ponderhit(std::move(limits));
    }
}

SearchService::SearchService(std::shared_ptr<const Evaluator> evaluator,
                             HashMemoryPolicy hash_memory_policy,
                             std::size_t initial_hash_mb) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires an evaluator");
    }
    impl_ = std::make_shared<Impl>(std::move(evaluator), std::move(hash_memory_policy),
                                   initial_hash_mb);
}

SearchHandle SearchService::start(GameState root, SearchLimits limits, SearchEventSink sink,
                                  SearchOptions options) {
    // The default SearchOptions hash value means "use the service configuration".
    // A non-default value explicitly reconfigures the shared service table.
    if (options.hash_mb != SearchOptions{}.hash_mb) {
        (void)impl_->table->set_size_mb(options.hash_mb);
    }

    options.threads = detail::normalized_threads(options.threads);
    options.speed_percent = detail::normalized_speed(options.speed_percent);
    options.multi_pv = detail::normalized_multi_pv(options.multi_pv);
    options.move_overhead_ms = detail::normalized_move_overhead(options.move_overhead_ms);
    options.slow_mover_percent = detail::normalized_slow_mover(options.slow_mover_percent);
    options.elo = detail::normalized_elo(options.elo);
    if (options.limit_strength && options.strength_profile_hook) {
        options.strength_profile_hook(options);
    }

    auto session = std::make_shared<detail::SearchSession>(
        std::move(root), std::move(limits), options);
    detail::SearchRunner runner(session, impl_->evaluator, impl_->table, impl_->evaluator_mutex,
                                std::move(sink));
    session->launch([runner = std::move(runner)]() mutable { runner.run(); });
    return SearchHandle(std::move(session));
}

void SearchService::set_evaluator(std::shared_ptr<const Evaluator> evaluator) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires a non-null evaluator");
    }
    impl_->evaluator = std::move(evaluator);
}

HashResizeResult SearchService::set_hash_size_mb(std::size_t megabytes) {
    return impl_->table->set_size_mb(megabytes);
}

std::size_t SearchService::hash_size_mb() const noexcept {
    return impl_->table->size_mb();
}

std::size_t SearchService::hashfull_permill() const noexcept {
    return impl_->table->hashfull_permill();
}

void SearchService::clear_hash() noexcept {
    impl_->table->clear();
}

} // namespace koi
