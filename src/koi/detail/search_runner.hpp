#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "koi/search_types.hpp"

namespace koi {

class Evaluator;
class TranspositionTable;

namespace detail {

class SearchSession;

// Option normalization is shared with SearchService::start so that the public
// entry point keeps clamping the request snapshot while the per-search flow
// itself lives in SearchRunner.
std::size_t normalized_threads(std::size_t threads) noexcept;
std::uint8_t normalized_speed(std::uint8_t speed_percent) noexcept;
std::size_t normalized_multi_pv(std::size_t multi_pv) noexcept;
std::uint32_t normalized_move_overhead(std::uint32_t move_overhead_ms) noexcept;
std::uint32_t normalized_slow_mover(std::uint32_t slow_mover_percent) noexcept;
std::uint32_t normalized_elo(std::uint32_t elo) noexcept;

// Owns the per-search flow that used to live inside the SearchService::start
// worker lambda: timing setup, tablebase probing, the serial and root-parallel
// iterative-deepening drivers, bounded fallbacks, and result publication.
// The runner holds exactly the dependencies the lambda captured; the session
// and table are shared so the worker keeps both alive for the search.
class SearchRunner {
public:
    SearchRunner(std::shared_ptr<SearchSession> session,
                 std::shared_ptr<const Evaluator> evaluator,
                 std::shared_ptr<TranspositionTable> table,
                 std::shared_ptr<std::mutex> evaluator_mutex,
                 SearchEventSink sink);

    void run();

private:
    std::shared_ptr<SearchSession> session_;
    std::shared_ptr<const Evaluator> evaluator_;
    std::shared_ptr<TranspositionTable> table_;
    std::shared_ptr<std::mutex> evaluator_mutex_;
    SearchEventSink sink_;
};

} // namespace detail

} // namespace koi
