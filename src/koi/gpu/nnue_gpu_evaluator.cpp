#include "koi/gpu/nnue_gpu_evaluator.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/gpu/gpu_nnue_service.hpp"

namespace koi::gpu {

namespace {

std::atomic<bool> g_threaded{false};

bool forced_failure() {
    static const bool forced = std::getenv("KOI_GPU_FORCE_FAIL") != nullptr;
    return forced;
}

std::size_t batch_capacity() {
    static const std::size_t capacity = [] {
        const char* value = std::getenv("KOI_GPU_BATCH");
        if (value == nullptr) {
            return std::size_t{256};
        }
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed < 1) {
            return std::size_t{256};
        }
        return static_cast<std::size_t>(parsed);
    }();
    return capacity;
}

// One in-flight evaluation request.  The leader of a batch owns nothing; the
// requesting thread keeps the request alive until its own flag is set.
struct PendingRequest {
    const GameState* state = nullptr;
    Color perspective = Color::white;
    std::shared_ptr<GpuNnueService> service;
    int score = 0;
    bool done = false;
    bool failed = false;
};

// Leader-follower batcher shared by every worker of every GPU evaluator in the
// process.  A leader drains up to `batch_capacity()` requests that share its
// service and evaluates them with one kernel launch; the others wait until
// their request completes.
struct BatchState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::unique_ptr<PendingRequest>> pending;
    bool in_flight = false;
};

BatchState& batch_state() {
    static BatchState state;
    return state;
}

} // namespace

bool gpu_nnue_available() noexcept {
    return true;
}

void set_gpu_nnue_threaded(const bool threaded) noexcept {
    g_threaded.store(threaded);
}

bool gpu_nnue_threaded() noexcept {
    return g_threaded.load();
}

GpuNnueEvaluator::GpuNnueEvaluator(std::shared_ptr<const Evaluator> fallback,
                                   std::shared_ptr<GpuNnueService> service)
    : fallback_(std::move(fallback)), service_(std::move(service)) {}

bool GpuNnueEvaluator::gpu_enabled() const noexcept {
    return service_ != nullptr && service_->available() && g_threaded.load() &&
        !forced_failure();
}

bool GpuNnueEvaluator::evaluate_on_gpu(const GameState& state, const Color perspective,
                                       int& score) const {
    if (!gpu_enabled()) {
        return false;
    }
    BatchState& shared = batch_state();
    std::unique_ptr<PendingRequest> owned = std::make_unique<PendingRequest>();
    owned->state = &state;
    owned->perspective = perspective;
    owned->service = service_;
    PendingRequest* active = owned.get();

    std::unique_lock lock(shared.mutex);
    shared.pending.push_back(std::move(owned));
    shared.cv.notify_all();
    for (;;) {
        if (active->done) {
            if (active->failed) {
                return false;
            }
            const Color mover = active->state->side_to_move();
            score = active->perspective == mover ? active->score : -active->score;
            return true;
        }
        if (!shared.in_flight) {
            shared.in_flight = true;
            std::vector<std::unique_ptr<PendingRequest>> batch;
            std::vector<const GameState*> states;
            std::vector<std::int32_t> scores;
            const std::size_t capacity = batch_capacity();
            while (!shared.pending.empty() && batch.size() < capacity) {
                const auto& candidate = shared.pending.back();
                if (candidate->service != service_) {
                    break;
                }
                std::unique_ptr<PendingRequest> taken = std::move(shared.pending.back());
                shared.pending.pop_back();
                states.push_back(taken->state);
                scores.push_back(0);
                batch.push_back(std::move(taken));
            }
            std::shared_ptr<GpuNnueService> service = service_;
            lock.unlock();
            std::string error;
            const bool ok = service != nullptr &&
                service->evaluate(
                    std::span<const GameState*>(states.data(), states.size()),
                    std::span<std::int32_t>(scores.data(), scores.size()), error);
            lock.lock();
            for (std::size_t index = 0; index < batch.size(); ++index) {
                batch[index]->done = true;
                batch[index]->failed = !ok;
                batch[index]->score = ok ? scores[index] : 0;
            }
            shared.in_flight = false;
            shared.cv.notify_all();
            continue;
        }
        shared.cv.wait(lock, [active] { return active->done; });
    }
}

int GpuNnueEvaluator::evaluate(const GameState& state, const Color perspective) const {
    int score = 0;
    if (evaluate_on_gpu(state, perspective, score)) {
        return score;
    }
    return fallback_ != nullptr ? fallback_->evaluate(state, perspective) : 0;
}

namespace {

class GpuNnueWorker final : public EvaluatorWorker {
public:
    GpuNnueWorker(const GpuNnueEvaluator& owner,
                  std::unique_ptr<EvaluatorWorker> fallback)
        : owner_(owner), fallback_(std::move(fallback)) {}

    [[nodiscard]] int evaluate(const GameState& state, const Color perspective) override {
        int score = 0;
        if (owner_.evaluate_on_gpu(state, perspective, score)) {
            return score;
        }
        if (fallback_ != nullptr) {
            return fallback_->evaluate(state, perspective);
        }
        return owner_.evaluate(state, perspective);
    }

    void on_make_move(const GameState& state, const MoveMetadata& metadata, const int ply,
                      const std::uint64_t parent_key) override {
        if (fallback_ != nullptr) {
            fallback_->on_make_move(state, metadata, ply, parent_key);
        }
    }
    void on_unmake_move(const int child_ply) override {
        if (fallback_ != nullptr) {
            fallback_->on_unmake_move(child_ply);
        }
    }
    void on_make_null_move(const GameState& state, const int ply,
                           const std::uint64_t parent_key) override {
        if (fallback_ != nullptr) {
            fallback_->on_make_null_move(state, ply, parent_key);
        }
    }
    void on_unmake_null_move(const int child_ply) override {
        if (fallback_ != nullptr) {
            fallback_->on_unmake_null_move(child_ply);
        }
    }

private:
    const GpuNnueEvaluator& owner_;
    std::unique_ptr<EvaluatorWorker> fallback_;
};

} // namespace

std::unique_ptr<EvaluatorWorker> GpuNnueEvaluator::create_worker() const {
    std::unique_ptr<EvaluatorWorker> fallback_worker;
    if (fallback_ != nullptr) {
        fallback_worker = fallback_->create_worker();
    }
    return std::make_unique<GpuNnueWorker>(*this, std::move(fallback_worker));
}

} // namespace koi::gpu
