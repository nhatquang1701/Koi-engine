#include "koi/search_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

#include "koi/detail/search_ordering.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi {
namespace {

constexpr int kInfinity = 1'000'000;
constexpr int kMateScore = 100'000;
constexpr int kMateThreshold = 99'000;
constexpr int kMaximumSearchDepth = 64;

std::optional<int> mate_from_score(int score) noexcept {
    if (score >= kMateThreshold) {
        return (kMateScore - score + 1) / 2;
    }
    if (score <= -kMateThreshold) {
        return -((kMateScore + score + 1) / 2);
    }
    return std::nullopt;
}

struct SearchContext {
    const Evaluator& evaluator;
    TranspositionTable& table;
    TimeManager time_manager;
    std::atomic_bool& stop_requested;
    detail::SearchMoveOrdering ordering;
    SearchStats stats;
    bool aborted = false;

    SearchContext(const Evaluator& evaluator, TranspositionTable& table, const SearchLimits& limits,
                  Color side_to_move, std::atomic_bool& stop_requested)
        : evaluator(evaluator), table(table), time_manager(limits, side_to_move), stop_requested(stop_requested) {}

    bool interrupted() {
        if (stop_requested.load(std::memory_order_relaxed) || time_manager.should_stop(stats.nodes + stats.qnodes)) {
            aborted = true;
            return true;
        }
        return false;
    }

    int terminal_score(const GameState& state, const std::vector<Move>& moves, int ply) const {
        if (!moves.empty()) {
            return 0;
        }
        return state.in_check() ? -kMateScore + ply : 0;
    }

    int quiescence(GameState& state, int alpha, int beta, int ply) {
        ++stats.qnodes;
        if (interrupted()) {
            return 0;
        }

        std::vector<Move> moves = state.legal_moves();
        if (moves.empty()) {
            return terminal_score(state, moves, ply);
        }
        if (state.is_terminal()) {
            return 0;
        }

        const bool checked = state.in_check();
        int best = -kInfinity;
        if (!checked) {
            best = evaluator.evaluate(state, state.side_to_move());
            if (best >= beta) {
                return best;
            }
            alpha = std::max(alpha, best);
            moves.erase(std::remove_if(moves.begin(), moves.end(), [&state](const Move& move) {
                return !state.is_capture(move) && move.promotion() == Promotion::none;
            }), moves.end());
            if (moves.empty()) {
                return best;
            }
        }

        ordering.order(state, moves, std::nullopt, ply);
        for (const Move& move : moves) {
            if (interrupted()) {
                return 0;
            }
            if (!state.make_move(move)) {
                continue;
            }
            const int score = -quiescence(state, -beta, -alpha, ply + 1);
            state.unmake_move();
            if (aborted) {
                return 0;
            }
            best = std::max(best, score);
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                break;
            }
        }
        return best;
    }

    int negamax(GameState& state, int depth, int alpha, int beta, int ply, std::vector<Move>& pv) {
        ++stats.nodes;
        if (interrupted()) {
            return 0;
        }

        std::vector<Move> moves = state.legal_moves();
        if (moves.empty()) {
            return terminal_score(state, moves, ply);
        }
        if (state.is_terminal()) {
            return 0;
        }
        if (depth <= 0) {
            return quiescence(state, alpha, beta, ply);
        }

        const int original_alpha = alpha;
        std::optional<Move> tt_move;
        if (const auto entry = table.probe(state.position_key(), ply); entry.has_value()) {
            ++stats.tt_hits;
            tt_move = entry->best_move.is_no_move() ? std::nullopt : std::optional<Move>{entry->best_move};
            if (ply > 0 && entry->depth >= depth) {
                if (entry->bound == TranspositionBound::exact) {
                    return entry->score;
                }
                if (entry->bound == TranspositionBound::lower) {
                    alpha = std::max(alpha, entry->score);
                } else {
                    beta = std::min(beta, entry->score);
                }
                if (alpha >= beta) {
                    return entry->score;
                }
            }
        }

        ordering.order(state, moves, tt_move, ply);
        int best_score = -kInfinity;
        Move best_move = Move::no_move();
        for (const Move& move : moves) {
            if (interrupted()) {
                return 0;
            }
            if (!state.make_move(move)) {
                continue;
            }
            std::vector<Move> child_pv;
            const int score = -negamax(state, depth - 1, -beta, -alpha, ply + 1, child_pv);
            state.unmake_move();
            if (aborted) {
                return 0;
            }
            if (score > best_score) {
                best_score = score;
                best_move = move;
                pv.clear();
                pv.push_back(move);
                pv.insert(pv.end(), child_pv.begin(), child_pv.end());
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (!state.is_capture(move) && move.promotion() == Promotion::none) {
                    ordering.record_quiet_cutoff(state.side_to_move(), move, ply, depth);
                }
                break;
            }
        }

        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= beta ? TranspositionBound::lower : TranspositionBound::exact;
        table.store(state.position_key(), depth, best_score, bound, best_move, ply);
        return best_score;
    }
};

void safely_report_info(const SearchEventSink& sink, const SearchInfo& info) {
    if (!sink.on_info) {
        return;
    }
    try {
        sink.on_info(info);
    } catch (...) {
    }
}

void safely_report_completion(const SearchEventSink& sink, const SearchResult& result) {
    if (!sink.on_complete) {
        return;
    }
    try {
        sink.on_complete(result);
    } catch (...) {
    }
}

} // namespace

struct SearchHandle::State {
    std::atomic_bool stop_requested = false;
    std::atomic_bool running = true;
    std::thread worker;
};

struct SearchService::Impl {
    explicit Impl(std::shared_ptr<const Evaluator> evaluator)
        : evaluator(std::move(evaluator)), table(std::make_shared<TranspositionTable>()) {}

    std::shared_ptr<const Evaluator> evaluator;
    std::shared_ptr<TranspositionTable> table;
};

SearchHandle::SearchHandle(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

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
        state_->stop_requested.store(true, std::memory_order_relaxed);
    }
}

void SearchHandle::wait() {
    if (state_ && state_->worker.joinable() && state_->worker.get_id() != std::this_thread::get_id()) {
        state_->worker.join();
    }
}

bool SearchHandle::running() const noexcept {
    return state_ && state_->running.load(std::memory_order_relaxed);
}

SearchService::SearchService(std::shared_ptr<const Evaluator> evaluator) {
    if (!evaluator) {
        throw std::invalid_argument("SearchService requires an evaluator");
    }
    impl_ = std::make_shared<Impl>(std::move(evaluator));
}

SearchHandle SearchService::start(GameState root, SearchLimits limits, SearchEventSink sink, SearchOptions options) {
    // The default SearchOptions value means "use the service configuration".
    // A non-default value explicitly reconfigures the shared service table.
    if (options.hash_mb != SearchOptions{}.hash_mb) {
        impl_->table->set_size_mb(options.hash_mb);
    }
    auto state = std::make_shared<SearchHandle::State>();
    const auto evaluator = impl_->evaluator;
    const auto table = impl_->table;
    state->worker = std::thread([state, evaluator, table, root = std::move(root), limits = std::move(limits), sink = std::move(sink)]() mutable {
        const auto started = std::chrono::steady_clock::now();
        table->new_generation();
        SearchContext context(*evaluator, *table, limits, root.side_to_move(), state->stop_requested);

        std::vector<Move> legal_moves = root.legal_moves();
        context.ordering.order(root, legal_moves, std::nullopt, 0);
        SearchResult result;
        result.best_move = legal_moves.empty() ? std::nullopt : std::optional<Move>{legal_moves.front()};
        result.score_cp = evaluator->evaluate(root, root.side_to_move());

        if (legal_moves.empty()) {
            result.score_cp = root.in_check() ? -kMateScore : 0;
            result.mate = mate_from_score(result.score_cp);
        } else if (root.is_terminal()) {
            result.score_cp = 0;
            result.best_move.reset();
        } else {
            const int maximum_depth = std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            for (int depth = 1; limits.infinite || depth <= maximum_depth;
                 depth = depth < maximum_depth ? depth + 1 : (limits.infinite ? maximum_depth : maximum_depth + 1)) {
                if (context.interrupted()) {
                    break;
                }
                std::vector<Move> pv;
                const int score = context.negamax(root, depth, -kInfinity, kInfinity, 0, pv);
                if (context.aborted) {
                    break;
                }

                result.completed_depth = depth;
                result.score_cp = score;
                result.mate = mate_from_score(score);
                if (!pv.empty()) {
                    result.best_move = pv.front();
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
                const std::uint64_t visited = context.stats.nodes + context.stats.qnodes;
                safely_report_info(sink, SearchInfo{depth, score, result.mate, context.stats.nodes,
                    elapsed.count() > 0 ? visited * 1000 / static_cast<std::uint64_t>(elapsed.count()) : visited,
                    elapsed, std::move(pv)});
            }
        }

        result.stats = context.stats;
        result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        result.mate = mate_from_score(result.score_cp);
        state->running.store(false, std::memory_order_release);
        safely_report_completion(sink, result);
    });
    return SearchHandle(std::move(state));
}

void SearchService::set_hash_size_mb(std::size_t megabytes) {
    impl_->table->set_size_mb(megabytes);
}

std::size_t SearchService::hash_size_mb() const noexcept {
    return impl_->table->size_mb();
}

void SearchService::clear_hash() noexcept {
    impl_->table->clear();
}

} // namespace koi
