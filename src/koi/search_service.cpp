#include "koi/search_service.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "koi/detail/search_ordering.hpp"
#include "koi/detail/static_exchange.hpp"
#include "koi/time_manager.hpp"
#include "koi/transposition_table.hpp"

namespace koi {
namespace {

constexpr int kInfinity = 1'000'000;
constexpr int kMateScore = 100'000;
constexpr int kMateThreshold = 99'000;
constexpr int kMaximumSearchDepth = 64;
constexpr int kMaximumQuiescenceDepth = 16;
constexpr int kMaximumQuiescenceSafetyDepth = 64;
constexpr int kMaximumQuiescenceCheckDepth = 2;
constexpr int kAspirationWindow = 50;
constexpr std::size_t kMaximumMultiPv = 16;

std::optional<int> mate_from_score(int score) noexcept {
    if (score >= kMateThreshold) {
        return (kMateScore - score + 1) / 2;
    }
    if (score <= -kMateThreshold) {
        return -((kMateScore + score + 1) / 2);
    }
    return std::nullopt;
}

std::size_t normalized_threads(std::size_t threads) noexcept {
    return std::clamp(threads, std::size_t{1}, maximum_search_threads());
}

std::uint8_t normalized_speed(std::uint8_t speed_percent) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::uint32_t>(speed_percent, 1, 100));
}

std::size_t normalized_multi_pv(std::size_t multi_pv) noexcept {
    return std::clamp(multi_pv, std::size_t{1}, kMaximumMultiPv);
}

constexpr int piece_value(PieceType type) noexcept {
    switch (type) {
    case PieceType::pawn:
        return 100;
    case PieceType::knight:
        return 320;
    case PieceType::bishop:
        return 330;
    case PieceType::rook:
        return 500;
    case PieceType::queen:
        return 900;
    case PieceType::king:
        return 20'000;
    case PieceType::none:
        return 0;
    }
    return 0;
}

constexpr std::size_t kMaximumPvLength = static_cast<std::size_t>(kMaximumSearchDepth);

struct PrincipalVariation {
    std::array<Move, kMaximumPvLength> moves{};
    std::uint8_t length = 0;

    void prepend(Move move, const PrincipalVariation& child) noexcept {
        const std::size_t child_length =
            std::min<std::size_t>(child.length, kMaximumPvLength - 1);
        moves[0] = move;
        std::copy_n(child.moves.data(), child_length, moves.data() + 1);
        length = static_cast<std::uint8_t>(child_length + 1);
    }

    [[nodiscard]] std::vector<Move> to_vector() const {
        return {moves.begin(), moves.begin() + length};
    }
};

void accumulate_stats(SearchStats& total, const SearchStats& partial) noexcept {
    total.nodes += partial.nodes;
    total.qnodes += partial.qnodes;
    total.tt_hits += partial.tt_hits;
    total.pvs_searches += partial.pvs_searches;
    total.pvs_researches += partial.pvs_researches;
    total.aspiration_researches += partial.aspiration_researches;
    total.check_extensions += partial.check_extensions;
    total.qchecks += partial.qchecks;
    total.see_prunes += partial.see_prunes;
    total.delta_prunes += partial.delta_prunes;
    total.null_cutoffs += partial.null_cutoffs;
    total.lmr_reductions += partial.lmr_reductions;
    total.seldepth = std::max(total.seldepth, partial.seldepth);
}

struct SearchContext {
    const Evaluator& evaluator;
    TranspositionTable& table;
    TimeManager& time_manager;
    std::atomic_bool& stop_requested;
    std::atomic<std::uint64_t>* global_nodes = nullptr;
    std::mutex* evaluator_mutex = nullptr;
    const MoveMetadataList* root_moves = nullptr;
    bool use_transposition_table = true;
    std::atomic_bool* iteration_aborted = nullptr;
    detail::SearchMoveOrdering ordering;
    SearchStats stats;
    bool aborted = false;

    SearchContext(const Evaluator& evaluator, TranspositionTable& table, TimeManager& time_manager,
                  std::atomic_bool& stop_requested,
                  std::atomic<std::uint64_t>* global_nodes = nullptr,
                  std::mutex* evaluator_mutex = nullptr,
                  const MoveMetadataList* root_moves = nullptr,
                  bool use_transposition_table = true)
        : evaluator(evaluator), table(table), time_manager(time_manager), stop_requested(stop_requested),
          global_nodes(global_nodes), evaluator_mutex(evaluator_mutex), root_moves(root_moves),
          use_transposition_table(use_transposition_table) {}

    void begin_iteration(std::atomic_bool* shared_abort) noexcept {
        stats = {};
        aborted = false;
        iteration_aborted = shared_abort;
    }

    void request_abort() noexcept {
        aborted = true;
        if (iteration_aborted != nullptr) {
            iteration_aborted->store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::uint64_t visited_nodes() const noexcept {
        if (global_nodes != nullptr) {
            return global_nodes->load(std::memory_order_relaxed);
        }
        return stats.nodes + stats.qnodes;
    }

    [[nodiscard]] bool reserve_global_node() noexcept {
        if (global_nodes == nullptr) {
            if (const std::optional<std::uint64_t> limit = time_manager.node_limit(); limit.has_value() &&
                stats.nodes + stats.qnodes >= *limit) {
                return false;
            }
            return true;
        }

        const std::optional<std::uint64_t> limit = time_manager.node_limit();
        if (!limit.has_value()) {
            global_nodes->fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        std::uint64_t observed = global_nodes->load(std::memory_order_relaxed);
        for (;;) {
            if (observed >= *limit) {
                return false;
            }
            if (global_nodes->compare_exchange_weak(observed, observed + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] bool interrupted() noexcept {
        if (stop_requested.load(std::memory_order_relaxed) ||
            (iteration_aborted != nullptr && iteration_aborted->load(std::memory_order_relaxed))) {
            aborted = true;
            return true;
        }
        if (time_manager.should_stop(visited_nodes())) {
            request_abort();
            return true;
        }
        return false;
    }

    [[nodiscard]] bool count_node(bool quiescence) noexcept {
        if (stop_requested.load(std::memory_order_relaxed) ||
            (iteration_aborted != nullptr && iteration_aborted->load(std::memory_order_relaxed))) {
            aborted = true;
            return false;
        }
        if (!reserve_global_node()) {
            request_abort();
            return false;
        }

        if (quiescence) {
            ++stats.qnodes;
        } else {
            ++stats.nodes;
        }
        return !interrupted();
    }

    [[nodiscard]] int terminal_score(const GameState& state, std::size_t move_count,
                                     int ply) const noexcept {
        if (move_count != 0) {
            return 0;
        }
        return state.in_check() ? -kMateScore + ply : 0;
    }

    [[nodiscard]] int evaluate(const GameState& state, Color perspective) const {
        if (evaluator_mutex != nullptr) {
            std::lock_guard lock(*evaluator_mutex);
            return evaluator.evaluate(state, perspective);
        }
        return evaluator.evaluate(state, perspective);
    }

    void record_ply(int ply) noexcept {
        stats.seldepth = std::max(stats.seldepth, ply);
    }

    int quiescence(GameState& state, int alpha, int beta, int ply, int qdepth = 0) {
        if (!count_node(true)) {
            return 0;
        }
        record_ply(ply);

        const bool checked = state.in_check();
        MoveMetadataList moves;
        const bool has_legal_move = checked ?
            (state.legal_moves_with_metadata(moves), !moves.empty()) :
            state.legal_tactical_moves_with_metadata(moves, qdepth < kMaximumQuiescenceCheckDepth);
        if (!has_legal_move) {
            return terminal_score(state, 0, ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }

        int best = -kInfinity;
        if (!checked) {
            best = evaluate(state, state.side_to_move());
            if (best >= beta) {
                return best;
            }
            alpha = std::max(alpha, best);
            if (qdepth >= kMaximumQuiescenceDepth) {
                return best;
            }
            if (moves.empty()) {
                return best;
            }
        } else if (qdepth >= kMaximumQuiescenceSafetyDepth) {
            // A checked position has no stand-pat score: the side to move must
            // play an evasion.  The safety limit only prevents pathological
            // perpetual-check trees from exhausting the call stack.
            return 0;
        }

        ordering.order(state, moves, std::nullopt, ply);
        for (const MoveMetadata& metadata : moves) {
            if (interrupted()) {
                return 0;
            }
            const Move move = metadata.move;
            if (!checked && metadata.is_capture() &&
                move.promotion() == Promotion::none && !metadata.gives_check) {
                if (detail::static_exchange_gain(state, metadata) < 0) {
                    ++stats.see_prunes;
                    continue;
                }
                const int delta = piece_value(metadata.captured_piece) + 100;
                if (best + delta < alpha) {
                    ++stats.delta_prunes;
                    continue;
                }
            }
            if (metadata.gives_check &&
                (checked || qdepth < kMaximumQuiescenceCheckDepth)) {
                ++stats.qchecks;
            }
            if (!state.make_legal_move(metadata)) {
                continue;
            }
            const int score = -quiescence(state, -beta, -alpha, ply + 1, qdepth + 1);
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

    int negamax(GameState& state, int depth, int alpha, int beta, int ply,
                PrincipalVariation& pv) {
        if (!count_node(false)) {
            return 0;
        }
        record_ply(ply);

        // Quiescence performs its own terminal-aware tactical/evasion move
        // generation. Do not build and annotate the full legal move list here
        // only to discard it immediately at the depth boundary.
        if (depth <= 0) {
            return quiescence(state, alpha, beta, ply);
        }

        const bool checked = state.in_check();
        MoveMetadataList moves;
        if (ply == 0 && root_moves != nullptr) {
            moves = *root_moves;
        } else {
            state.legal_moves_with_metadata(moves, false);
        }
        if (moves.empty()) {
            return terminal_score(state, moves.size(), ply);
        }
        if (state.is_draw_by_rule()) {
            return 0;
        }
        if (checked && depth < kMaximumSearchDepth) {
            ++stats.check_extensions;
            ++depth;
        }

        const int original_alpha = alpha;
        const int original_beta = beta;
        std::optional<Move> tt_move;
        if (use_transposition_table) {
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
        }

        if (!checked && depth >= 3 && beta < kInfinity && beta > -kInfinity &&
            beta - alpha <= 1 && state.has_non_pawn_material(state.side_to_move())) {
            if (state.make_null_move()) {
                PrincipalVariation null_pv;
                const int reduction = depth >= 6 ? 3 : 2;
                const int null_depth = std::max(0, depth - 1 - reduction);
                const int null_score = -negamax(state, null_depth, -beta, -beta + 1, ply + 1, null_pv);
                state.unmake_null_move();
                if (aborted) {
                    return 0;
                }
                if (null_score >= beta) {
                    ++stats.null_cutoffs;
                    if (use_transposition_table) {
                        table.store(state.position_key(), depth, null_score,
                                    TranspositionBound::lower, Move::no_move(), ply);
                    }
                    return null_score;
                }
            }
        }

        ordering.order(state, moves, tt_move, ply);
        int best_score = -kInfinity;
        Move best_move = Move::no_move();
        int move_number = 0;
        for (const MoveMetadata& metadata : moves) {
            if (interrupted()) {
                return 0;
            }
            const Move move = metadata.move;
            if (!state.make_legal_move(metadata)) {
                continue;
            }

            const bool reduced = move_number >= 4 && depth >= 4 && !checked && !state.in_check() &&
                !metadata.is_capture() && move.promotion() == Promotion::none;
            const int full_child_depth = depth - 1;
            const int child_depth = reduced ? std::max(0, full_child_depth - 1) : full_child_depth;
            if (reduced) {
                ++stats.lmr_reductions;
            }

            PrincipalVariation child_pv;
            int score = 0;
            if (move_number == 0) {
                score = -negamax(state, child_depth, -beta, -alpha, ply + 1, child_pv);
            } else {
                ++stats.pvs_searches;
                score = -negamax(state, child_depth, -alpha - 1, -alpha, ply + 1, child_pv);
                if (!aborted && score > alpha && score < beta) {
                    ++stats.pvs_researches;
                    child_pv = {};
                    score = -negamax(state, full_child_depth, -beta, -alpha, ply + 1, child_pv);
                }
            }
            state.unmake_move();
            if (aborted) {
                return 0;
            }

            if (score > best_score) {
                best_score = score;
                best_move = move;
                pv.prepend(move, child_pv);
            }
            alpha = std::max(alpha, score);
            if (alpha >= beta) {
                if (!metadata.is_capture() && move.promotion() == Promotion::none) {
                    ordering.record_quiet_cutoff(state.side_to_move(), move, ply, depth);
                }
                break;
            }
            ++move_number;
        }

        const TranspositionBound bound = best_score <= original_alpha ? TranspositionBound::upper
            : best_score >= original_beta ? TranspositionBound::lower : TranspositionBound::exact;
        if (use_transposition_table) {
            table.store(state.position_key(), depth, best_score, bound, best_move, ply);
        }
        return best_score;
    }
};

struct RootLine {
    bool completed = false;
    int score = -kInfinity;
    std::size_t stable_index = 0;
    PrincipalVariation pv;
};

class RootWorkerPool {
public:
    RootWorkerPool(std::size_t worker_count, const Evaluator& evaluator, TranspositionTable& table,
                   TimeManager& time_manager, std::atomic_bool& stop_requested,
                   std::atomic<std::uint64_t>* global_nodes, std::mutex* evaluator_mutex,
                   bool use_transposition_table)
        : worker_count_(std::max<std::size_t>(1, worker_count)), evaluator_(evaluator), table_(table),
          time_manager_(time_manager), stop_requested_(stop_requested), global_nodes_(global_nodes),
          evaluator_mutex_(evaluator_mutex), use_transposition_table_(use_transposition_table),
          worker_stats_(worker_count_) {
        workers_.reserve(worker_count_);
        try {
            for (std::size_t index = 0; index < worker_count_; ++index) {
                workers_.emplace_back(&RootWorkerPool::worker_loop, this, index);
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    RootWorkerPool(const RootWorkerPool&) = delete;
    RootWorkerPool& operator=(const RootWorkerPool&) = delete;

    ~RootWorkerPool() {
        shutdown();
    }

    void run(const GameState& root, const MoveMetadataList& root_moves, int depth,
             int alpha, int beta, const std::vector<std::size_t>& stable_root_indices,
             std::vector<RootLine>& lines,
             SearchStats& stats, bool& aborted) {
        auto job = std::make_shared<Job>();
        job->root = &root;
        job->root_moves = &root_moves;
        job->depth = depth;
        job->alpha = alpha;
        job->beta = beta;
        job->stable_root_indices = &stable_root_indices;
        job->lines = &lines;
        job->worker_stats = &worker_stats_;
        // Complete the stable PV/root prefix before sharing the remaining roots. This lets
        // the preferred root warm the transposition table for the parallel remainder.
        job->priority_root_count = std::min<std::size_t>(1, root_moves.size());
        job->next_move.store(job->priority_root_count, std::memory_order_relaxed);

        {
            std::lock_guard lock(mutex_);
            ++sequence_;
            job->sequence = sequence_;
            finished_workers_ = 0;
            job_ = job;
        }
        work_available_.notify_all();

        {
            std::unique_lock lock(mutex_);
            work_finished_.wait(lock, [this] { return finished_workers_ == worker_count_; });
        }

        stats = {};
        for (const SearchStats& worker_stats : worker_stats_) {
            accumulate_stats(stats, worker_stats);
        }
        aborted = job->aborted.load(std::memory_order_relaxed) ||
            stop_requested_.load(std::memory_order_relaxed);
    }

private:
    struct Job {
        const GameState* root = nullptr;
        const MoveMetadataList* root_moves = nullptr;
        int depth = 0;
        int alpha = -kInfinity;
        int beta = kInfinity;
        const std::vector<std::size_t>* stable_root_indices = nullptr;
        std::vector<RootLine>* lines = nullptr;
        std::vector<SearchStats>* worker_stats = nullptr;
        std::size_t priority_root_count = 0;
        std::atomic<std::size_t> next_move = 0;
        std::atomic_bool aborted = false;
        std::atomic_bool priority_ready = false;
        std::uint64_t sequence = 0;
    };

    void worker_loop(std::size_t worker_index) {
        // Single-PV root jobs are speculative. Keeping their TT activity local prevents a racing
        // null-window result from changing the serial iteration that is ultimately published.
        SearchContext context(evaluator_, table_, time_manager_, stop_requested_, global_nodes_,
                              evaluator_mutex_, nullptr, use_transposition_table_);
        std::uint64_t seen_sequence = 0;

        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this, seen_sequence] {
                    return stopping_ || (job_ != nullptr && job_->sequence != seen_sequence);
                });
                if (stopping_) {
                    return;
                }
                job = job_;
            }

            seen_sequence = job->sequence;
            context.begin_iteration(&job->aborted);

            const auto search_root = [&context, &job](std::size_t move_index) {
                const MoveMetadata& root_move = (*job->root_moves)[move_index];
                try {
                    GameState child = *job->root;
                    if (!child.make_legal_move(root_move)) {
                        return;
                    }

                    PrincipalVariation child_pv;
                    const int score = -context.negamax(child, job->depth - 1,
                                                       -job->beta, -job->alpha, 1, child_pv);
                    if (context.aborted) {
                        job->aborted.store(true, std::memory_order_relaxed);
                        return;
                    }

                    RootLine& line = (*job->lines)[move_index];
                    line.score = score;
                    line.stable_index = (*job->stable_root_indices)[move_index];
                    line.pv.prepend(root_move.move, child_pv);
                    line.completed = true;
                } catch (...) {
                    context.request_abort();
                    job->aborted.store(true, std::memory_order_relaxed);
                }
            };

            if (worker_index == 0) {
                for (std::size_t move_index = 0; move_index < job->priority_root_count; ++move_index) {
                    if (job->aborted.load(std::memory_order_relaxed) ||
                        stop_requested_.load(std::memory_order_relaxed)) {
                        break;
                    }
                    search_root(move_index);
                }
                job->priority_ready.store(true, std::memory_order_release);
                work_available_.notify_all();
            } else {
                std::unique_lock lock(mutex_);
                work_available_.wait(lock, [this, job] {
                    return stopping_ || job->priority_ready.load(std::memory_order_acquire);
                });
                if (stopping_) {
                    return;
                }
            }

            while (!job->aborted.load(std::memory_order_relaxed) &&
                   !stop_requested_.load(std::memory_order_relaxed)) {
                const std::size_t move_index = job->next_move.fetch_add(1, std::memory_order_relaxed);
                if (move_index >= job->root_moves->size()) {
                    break;
                }
                search_root(move_index);
            }

            (*job->worker_stats)[worker_index] = context.stats;
            {
                std::lock_guard lock(mutex_);
                ++finished_workers_;
            }
            work_finished_.notify_one();
        }
    }

    void shutdown() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        work_available_.notify_all();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    const std::size_t worker_count_;
    const Evaluator& evaluator_;
    TranspositionTable& table_;
    TimeManager& time_manager_;
    std::atomic_bool& stop_requested_;
    std::atomic<std::uint64_t>* global_nodes_;
    std::mutex* evaluator_mutex_;
    bool use_transposition_table_;
    std::vector<std::thread> workers_;
    std::vector<SearchStats> worker_stats_;
    std::mutex mutex_;
    std::condition_variable work_available_;
    std::condition_variable work_finished_;
    std::shared_ptr<Job> job_;
    std::size_t finished_workers_ = 0;
    std::uint64_t sequence_ = 0;
    bool stopping_ = false;
};

std::vector<std::size_t> rank_root_lines(const std::vector<RootLine>& lines) {
    std::vector<std::size_t> ranked;
    ranked.reserve(lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].completed) {
            ranked.push_back(index);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [&lines](std::size_t left, std::size_t right) {
        if (lines[left].score != lines[right].score) {
            return lines[left].score > lines[right].score;
        }
        return lines[left].stable_index < lines[right].stable_index;
    });
    return ranked;
}

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
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    std::thread worker;
};

struct SearchService::Impl {
    explicit Impl(std::shared_ptr<const Evaluator> evaluator)
        : evaluator(std::move(evaluator)), table(std::make_shared<TranspositionTable>()),
          evaluator_mutex(std::make_shared<std::mutex>()) {}

    std::shared_ptr<const Evaluator> evaluator;
    std::shared_ptr<TranspositionTable> table;
    std::shared_ptr<std::mutex> evaluator_mutex;
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
        state_->stop_condition.notify_all();
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

SearchHandle SearchService::start(GameState root, SearchLimits limits, SearchEventSink sink,
                                  SearchOptions options) {
    // The default SearchOptions hash value means "use the service configuration".
    // A non-default value explicitly reconfigures the shared service table.
    if (options.hash_mb != SearchOptions{}.hash_mb) {
        impl_->table->set_size_mb(options.hash_mb);
    }

    options.threads = normalized_threads(options.threads);
    options.speed_percent = normalized_speed(options.speed_percent);
    options.multi_pv = normalized_multi_pv(options.multi_pv);

    auto state = std::make_shared<SearchHandle::State>();
    const auto evaluator = impl_->evaluator;
    const auto table = impl_->table;
    const auto evaluator_mutex = impl_->evaluator_mutex;
    state->worker = std::thread([state, evaluator, table, root = std::move(root), limits = std::move(limits),
                                 sink = std::move(sink), options, evaluator_mutex]() mutable {
        const auto started = std::chrono::steady_clock::now();
        table->new_generation();
        TimeManager time_manager(limits, root.side_to_move(), options.speed_percent);

        std::mutex* evaluator_mutex_ptr = evaluator->supports_concurrent_evaluation() ?
            nullptr : evaluator_mutex.get();

        SearchResult result;
        MoveMetadataList legal_moves;
        root.legal_moves_with_metadata(legal_moves);
        if (limits.search_moves_specified) {
            MoveMetadataList filtered_moves;
            for (const MoveMetadata& metadata : legal_moves) {
                if (std::find(limits.search_moves.begin(), limits.search_moves.end(), metadata.move) !=
                    limits.search_moves.end()) {
                    (void)filtered_moves.push_back(metadata);
                }
            }
            legal_moves = filtered_moves;
        }
        result.best_move = legal_moves.empty() ? std::nullopt :
            std::optional<Move>{legal_moves.front().move};
        if (evaluator_mutex_ptr != nullptr) {
            std::lock_guard lock(*evaluator_mutex_ptr);
            result.score_cp = evaluator->evaluate(root, root.side_to_move());
        } else {
            result.score_cp = evaluator->evaluate(root, root.side_to_move());
        }

        if (legal_moves.empty()) {
            result.score_cp = root.in_check() ? -kMateScore : 0;
            result.mate = mate_from_score(result.score_cp);
        } else if (root.is_draw_by_rule()) {
            result.score_cp = 0;
            result.best_move.reset();
        } else if ((options.threads == 1 && options.multi_pv == 1) || legal_moves.size() < 2 ||
                   (time_manager.node_limit().has_value() && options.multi_pv == 1)) {
            SearchContext context(*evaluator, *table, time_manager, state->stop_requested,
                                  nullptr, evaluator_mutex_ptr, &legal_moves);
            context.ordering.order(root, legal_moves, std::nullopt, 0);
            result.best_move = legal_moves.front().move;

            const int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            const bool unbounded = limits.infinite || limits.ponder;
            std::optional<int> previous_score;
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                if (context.interrupted()) {
                    break;
                }
                PrincipalVariation pv;
                int alpha = -kInfinity;
                int beta = kInfinity;
                if (previous_score.has_value()) {
                    alpha = std::max(-kInfinity, *previous_score - kAspirationWindow);
                    beta = std::min(kInfinity, *previous_score + kAspirationWindow);
                }

                int score = context.negamax(root, depth, alpha, beta, 0, pv);
                if (!context.aborted && previous_score.has_value() &&
                    (score <= alpha || score >= beta)) {
                    ++context.stats.aspiration_researches;
                    pv = {};
                    score = context.negamax(root, depth, -kInfinity, kInfinity, 0, pv);
                }
                if (context.aborted) {
                    break;
                }

                result.completed_depth = depth;
                result.score_cp = score;
                result.mate = mate_from_score(score);
                if (pv.length > 0) {
                    result.best_move = pv.moves[0];
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                const std::uint64_t visited = context.stats.nodes + context.stats.qnodes;
                SearchInfo info{depth, score, result.mate, visited,
                                elapsed.count() > 0 ? visited * 1000 /
                                    static_cast<std::uint64_t>(elapsed.count()) : visited,
                                elapsed, pv.to_vector()};
                info.seldepth = context.stats.seldepth;
                info.qnodes = context.stats.qnodes;
                info.tt_hits = context.stats.tt_hits;
                safely_report_info(sink, info);
                previous_score = score;

                if (!unbounded && depth == maximum_depth) {
                    break;
                }
            }
            result.stats = context.stats;
        } else {
            std::atomic<std::uint64_t> global_nodes = 0;
            std::atomic<std::uint64_t>* global_nodes_ptr = time_manager.node_limit().has_value() ?
                &global_nodes : nullptr;
            const std::size_t worker_count = std::min(options.threads, legal_moves.size());
            const bool multi_pv = options.multi_pv > 1;
            RootWorkerPool pool(worker_count, *evaluator, *table, time_manager,
                                state->stop_requested, global_nodes_ptr, evaluator_mutex_ptr, multi_pv);
            SearchStats total_stats;
            SearchContext root_context(*evaluator, *table, time_manager, state->stop_requested,
                                       global_nodes_ptr, evaluator_mutex_ptr);
            MoveMetadataList reference_moves = legal_moves;
            SearchContext reference_context(*evaluator, *table, time_manager, state->stop_requested,
                                            nullptr, evaluator_mutex_ptr, &reference_moves);
            if (!multi_pv) {
                reference_context.ordering.order(root, reference_moves, std::nullopt, 0);
            }

            MoveMetadataList parallel_moves = legal_moves;
            detail::SearchMoveOrdering root_ordering;
            root_ordering.order(root, parallel_moves, std::nullopt, 0);
            const int maximum_depth =
                std::min(kMaximumSearchDepth, std::max(1, limits.depth.value_or(kMaximumSearchDepth)));
            const bool unbounded = limits.infinite || limits.ponder;
            std::optional<int> previous_score;
            for (int depth = 1;; depth = depth < maximum_depth ? depth + 1 : maximum_depth) {
                if (!unbounded && depth > maximum_depth) {
                    break;
                }
                if (state->stop_requested.load(std::memory_order_relaxed) ||
                    time_manager.should_stop(root_context.visited_nodes())) {
                    break;
                }

                root_context.begin_iteration(nullptr);
                if (!root_context.count_node(false)) {
                    accumulate_stats(total_stats, root_context.stats);
                    break;
                }

                std::optional<Move> tt_move;
                if (const auto entry = table->probe(root.position_key(), 0); entry.has_value()) {
                    ++root_context.stats.tt_hits;
                    if (!entry->best_move.is_no_move()) {
                        tt_move = entry->best_move;
                    }
                }
                root_ordering.order(root, parallel_moves, tt_move, 0);
                std::vector<std::size_t> stable_root_indices;
                stable_root_indices.reserve(parallel_moves.size());
                for (std::size_t index = 0; index < parallel_moves.size(); ++index) {
                    stable_root_indices.push_back(index);
                }

                std::vector<RootLine> lines(parallel_moves.size());
                int alpha = -kInfinity;
                int beta = kInfinity;
                if (!multi_pv && previous_score.has_value()) {
                    alpha = std::max(-kInfinity, *previous_score - kAspirationWindow);
                    beta = std::min(kInfinity, *previous_score + kAspirationWindow);
                }

                SearchStats iteration_stats;
                bool aborted = false;
                pool.run(root, parallel_moves, depth, alpha, beta, stable_root_indices,
                         lines, iteration_stats, aborted);
                accumulate_stats(iteration_stats, root_context.stats);

                if (!multi_pv && !aborted && previous_score.has_value()) {
                    int best_score = -kInfinity;
                    bool failed_high = false;
                    for (const RootLine& line : lines) {
                        if (!line.completed) {
                            continue;
                        }
                        best_score = std::max(best_score, line.score);
                        failed_high = failed_high || line.score >= beta;
                    }
                    if (best_score <= alpha || failed_high) {
                        ++iteration_stats.aspiration_researches;
                        lines.assign(parallel_moves.size(), RootLine{});
                        SearchStats research_stats;
                        bool research_aborted = false;
                        pool.run(root, parallel_moves, depth, -kInfinity, kInfinity,
                                 stable_root_indices, lines, research_stats, research_aborted);
                        accumulate_stats(iteration_stats, research_stats);
                        aborted = research_aborted;
                    }
                }
                accumulate_stats(total_stats, iteration_stats);
                if (aborted) {
                    break;
                }

                if (!multi_pv) {
                    PrincipalVariation pv;
                    int score = reference_context.negamax(root, depth, alpha, beta, 0, pv);
                    if (!reference_context.aborted && previous_score.has_value() &&
                        (score <= alpha || score >= beta)) {
                        ++reference_context.stats.aspiration_researches;
                        pv = {};
                        score = reference_context.negamax(root, depth, -kInfinity, kInfinity, 0, pv);
                    }
                    if (reference_context.aborted) {
                        break;
                    }

                    result.completed_depth = depth;
                    result.score_cp = score;
                    result.mate = mate_from_score(score);
                    if (pv.length > 0) {
                        result.best_move = pv.moves[0];
                    }

                    SearchStats reported_stats = total_stats;
                    accumulate_stats(reported_stats, reference_context.stats);
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                    const std::uint64_t visited = reported_stats.nodes + reported_stats.qnodes;
                    SearchInfo info{depth, score, result.mate, visited,
                                    elapsed.count() > 0 ? visited * 1000 /
                                        static_cast<std::uint64_t>(elapsed.count()) : visited,
                                    elapsed, pv.to_vector()};
                    info.seldepth = reported_stats.seldepth;
                    info.qnodes = reported_stats.qnodes;
                    info.tt_hits = reported_stats.tt_hits;
                    safely_report_info(sink, info);
                    previous_score = score;

                    if (!unbounded && depth == maximum_depth) {
                        break;
                    }
                    continue;
                }

                std::vector<std::size_t> ranked_indices;
                if (multi_pv) {
                    ranked_indices = rank_root_lines(lines);
                } else {
                    ranked_indices.reserve(lines.size());
                    for (std::size_t index = 0; index < lines.size(); ++index) {
                        if (lines[index].completed) {
                            ranked_indices.push_back(index);
                        }
                    }
                    std::stable_sort(ranked_indices.begin(), ranked_indices.end(),
                                     [&lines](std::size_t left, std::size_t right) {
                                         return lines[left].score > lines[right].score;
                                     });
                }
                if (ranked_indices.empty()) {
                    break;
                }

                const std::size_t best_index = ranked_indices.front();
                const RootLine& best_line = lines[best_index];

                result.completed_depth = depth;
                result.score_cp = best_line.score;
                result.mate = mate_from_score(best_line.score);
                result.best_move = best_line.pv.length > 0 ?
                    std::optional<Move>{best_line.pv.moves[0]} : result.best_move;

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                const std::uint64_t visited = total_stats.nodes + total_stats.qnodes;
                const std::uint64_t nps = elapsed.count() > 0 ? visited * 1000 /
                    static_cast<std::uint64_t>(elapsed.count()) : visited;
                const std::size_t line_count = multi_pv ?
                    std::min(options.multi_pv, ranked_indices.size()) : std::size_t{1};
                for (std::size_t rank = 0; rank < line_count; ++rank) {
                    const RootLine& line = lines[ranked_indices[rank]];
                    SearchInfo info{depth, line.score, mate_from_score(line.score), visited,
                                    nps, elapsed, line.pv.to_vector()};
                    info.seldepth = total_stats.seldepth;
                    info.qnodes = total_stats.qnodes;
                    info.tt_hits = total_stats.tt_hits;
                    info.multipv = static_cast<int>(rank + 1);
                    safely_report_info(sink, info);
                }
                previous_score = best_line.score;

                if (!unbounded && depth == maximum_depth) {
                    break;
                }
            }
            result.stats = total_stats;
            if (!multi_pv) {
                accumulate_stats(result.stats, reference_context.stats);
            }
        }

        if (limits.ponder && (legal_moves.empty() || root.is_draw_by_rule())) {
            std::unique_lock lock(state->stop_mutex);
            state->stop_condition.wait(lock, [state] {
                return state->stop_requested.load(std::memory_order_relaxed);
            });
        }

        result.stats.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
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
