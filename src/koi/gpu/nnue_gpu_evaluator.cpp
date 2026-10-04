#include "koi/gpu/nnue_gpu_evaluator.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/gpu/gpu_nnue_service.hpp"

// The kernel macro is defined (0 or 1) by the build; treat an undefined value
// like a CPU-only build so this TU also compiles in the macOS configuration,
// which turns GPU inference off.
#if defined(KOI_GPU_INFERENCE_AVAILABLE) && KOI_GPU_INFERENCE_AVAILABLE
#define KOI_NNUE_GPU_DRIVER_AVAILABLE 1
#include "koi/gpu/cuda_driver.hpp"
#else
#define KOI_NNUE_GPU_DRIVER_AVAILABLE 0
#endif

namespace koi::gpu {

namespace {

std::atomic<int> g_threaded_searches{0};
std::atomic<GpuNnueMode> g_requested_mode{GpuNnueMode::automatic};

// 0 = not probed yet, 1 = no override, 2 = disabled, 3 = require.
std::atomic<int> g_env_override{0};

std::mutex g_status_mutex;
GpuNnueStatus g_status;

bool forced_failure() {
    static const bool forced = std::getenv("KOI_GPU_FORCE_FAIL") != nullptr;
    return forced;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
        }
    }
    return true;
}

std::size_t batch_capacity() {
    static const std::size_t capacity = [] {
        const char* value = std::getenv("KOI_GPU_BATCH");
        if (value == nullptr) {
            return kMaximumGpuBatchSize;
        }
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed < 1) {
            return kMaximumGpuBatchSize;
        }
        // The service stages a fixed number of positions per launch, so a
        // larger configured batch would fail on every evaluation.
        return std::min(static_cast<std::size_t>(parsed), kMaximumGpuBatchSize);
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
// their request completes.  Requests are shared_ptr-owned so that a completed
// batch can release its references while the waiting threads still hold theirs.
struct BatchState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::shared_ptr<PendingRequest>> pending;
    // Leader scratch, reused across batches so a steady stream of requests does
    // not allocate three vectors per launch.  Only the single leader (the
    // thread that observed `in_flight == false`) touches these while the mutex
    // is released.
    std::vector<std::shared_ptr<PendingRequest>> batch;
    std::vector<const GameState*> batch_states;
    std::vector<std::int32_t> batch_scores;
    bool in_flight = false;
};

BatchState& batch_state() {
    static BatchState state;
    return state;
}

} // namespace

std::string_view gpu_nnue_mode_name(const GpuNnueMode mode) noexcept {
    switch (mode) {
    case GpuNnueMode::automatic:
        return "auto";
    case GpuNnueMode::require:
        return "on";
    case GpuNnueMode::disabled:
        return "off";
    }
    return "auto";
}

bool parse_gpu_nnue_mode(const std::string_view text, GpuNnueMode& mode) noexcept {
    if (equals_ignore_case(text, "auto")) {
        mode = GpuNnueMode::automatic;
        return true;
    }
    if (equals_ignore_case(text, "on")) {
        mode = GpuNnueMode::require;
        return true;
    }
    if (equals_ignore_case(text, "off")) {
        mode = GpuNnueMode::disabled;
        return true;
    }
    return false;
}

GpuNnueMode gpu_nnue_mode() noexcept {
    return g_requested_mode.load(std::memory_order_relaxed);
}

void set_gpu_nnue_mode(const GpuNnueMode mode) noexcept {
    g_requested_mode.store(mode, std::memory_order_relaxed);
}

std::optional<GpuNnueMode> gpu_nnue_env_override() noexcept {
    int state = g_env_override.load(std::memory_order_acquire);
    if (state == 0) {
        int computed = 1;
        if (const char* value = std::getenv("KOI_GPU_NNUE");
            value != nullptr && *value != '\0') {
            computed = std::string_view(value) == "0" ? 2 : 3;
        }
        int expected = 0;
        if (g_env_override.compare_exchange_strong(expected, computed,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire)) {
            state = computed;
        } else {
            state = expected;
        }
    }
    switch (state) {
    case 2:
        return GpuNnueMode::disabled;
    case 3:
        return GpuNnueMode::require;
    default:
        return std::nullopt;
    }
}

void reset_gpu_nnue_environment_cache() noexcept {
    g_env_override.store(0, std::memory_order_release);
}

GpuNnueMode effective_gpu_nnue_mode() noexcept {
    const std::optional<GpuNnueMode> override_mode = gpu_nnue_env_override();
    const GpuNnueMode requested = gpu_nnue_mode();
    // An explicit off from either source is absolute; otherwise the environment
    // override outranks the option.
    if (requested == GpuNnueMode::disabled ||
        (override_mode.has_value() && *override_mode == GpuNnueMode::disabled)) {
        return GpuNnueMode::disabled;
    }
    if (override_mode.has_value() && *override_mode == GpuNnueMode::require) {
        return GpuNnueMode::require;
    }
    return requested;
}

bool gpu_nnue_available() noexcept {
#if KOI_NNUE_GPU_DRIVER_AVAILABLE
    // Probing loads the CUDA driver and opens a context; cache the result so
    // the option handler and the per-search diagnostic stay cheap.  No kernel
    // runs, so a device busy with unrelated work is left alone.
    static const bool available = []() noexcept {
        try {
            std::string error;
            return CudaDriver::open(error).available();
        } catch (...) {
            return false;
        }
    }();
    return available;
#else
    return false;
#endif
}

void record_gpu_nnue_status(const bool network_loaded, const bool service_ready,
                            std::string device_name, std::string reason) noexcept {
    std::lock_guard lock(g_status_mutex);
    g_status.network_loaded = network_loaded;
    g_status.service_ready = service_ready;
    g_status.device_name = std::move(device_name);
    g_status.reason = std::move(reason);
}

GpuNnueStatus gpu_nnue_status() {
    std::lock_guard lock(g_status_mutex);
    return g_status;
}

std::string gpu_nnue_search_diagnostic(const std::size_t threads) {
    const GpuNnueStatus status = gpu_nnue_status();
    if (!status.network_loaded) {
        return {};
    }
    const std::optional<GpuNnueMode> override_mode = gpu_nnue_env_override();
    const GpuNnueMode effective = effective_gpu_nnue_mode();
    const bool overridden = override_mode.has_value() &&
        *override_mode != gpu_nnue_mode();
    if (effective == GpuNnueMode::disabled) {
        if (override_mode.has_value() && *override_mode == GpuNnueMode::disabled) {
            return "GPU NNUE: off (KOI_GPU_NNUE=0 override)";
        }
        return "GPU NNUE: off (GpuNnue=off)";
    }
    // The GPU path only serves multi-threaded searches so Threads=1 keeps the
    // deterministic CPU implementation, exactly as before the option existed.
    if (threads <= 1) {
        return "GPU NNUE: off (Threads=1)";
    }
    if (forced_failure()) {
        return "GPU NNUE: off (KOI_GPU_FORCE_FAIL)";
    }
    if (status.service_ready) {
        std::string line = "GPU NNUE: ";
        line += gpu_nnue_mode_name(effective);
        line += " -> cuda";
        if (!status.device_name.empty()) {
            line += " (";
            line += status.device_name;
            line += ')';
        }
        if (overridden) {
            line += " [KOI_GPU_NNUE override]";
        }
        return line;
    }
    std::string reason = status.reason;
    if (!gpu_nnue_available()) {
        // This build or machine cannot run the kernel at all; a reason recorded
        // earlier (for example while the option was off) would be stale.
#if KOI_NNUE_GPU_DRIVER_AVAILABLE
        reason = "no CUDA driver";
#else
        reason = "this build has no GPU NNUE kernel";
#endif
    } else if (reason.empty()) {
        reason = "the GPU NNUE service is unavailable";
    }
    if (effective == GpuNnueMode::require) {
        return "GPU NNUE: unavailable (" + reason + ")";
    }
    return "GPU NNUE: auto -> cpu (" + reason + ")";
}

void begin_gpu_nnue_threaded_search() noexcept {
    g_threaded_searches.fetch_add(1, std::memory_order_relaxed);
}

void end_gpu_nnue_threaded_search() noexcept {
    // Saturate at zero: an unmatched release must not turn the counter negative
    // and permanently disable (or re-enable) the GPU path.
    int current = g_threaded_searches.load(std::memory_order_relaxed);
    while (current > 0 &&
           !g_threaded_searches.compare_exchange_weak(current, current - 1,
                                                      std::memory_order_relaxed)) {
    }
}

bool gpu_nnue_threaded() noexcept {
    return g_threaded_searches.load(std::memory_order_relaxed) > 0;
}

GpuNnueEvaluator::GpuNnueEvaluator(std::shared_ptr<const Evaluator> fallback,
                                   std::shared_ptr<GpuNnueService> service)
    : fallback_(std::move(fallback)), service_(std::move(service)) {}

bool GpuNnueEvaluator::gpu_enabled() const noexcept {
    // The mode is re-read on every evaluation so a `setoption name GpuNnue`
    // (which stops and joins the active search first) takes effect for the next
    // search without rebuilding the evaluator.
    return effective_gpu_nnue_mode() != GpuNnueMode::disabled &&
        service_ != nullptr && service_->available() && gpu_nnue_threaded() &&
        !forced_failure();
}

bool GpuNnueEvaluator::evaluate_on_gpu(const GameState& state, const Color perspective,
                                       int& score) const {
    if (!gpu_enabled()) {
        return false;
    }
    BatchState& shared = batch_state();
    std::shared_ptr<PendingRequest> owned = std::make_shared<PendingRequest>();
    owned->state = &state;
    owned->perspective = perspective;
    owned->service = service_;
    PendingRequest* active = owned.get();

    std::unique_lock lock(shared.mutex);
    shared.pending.push_back(owned);
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
            shared.batch.clear();
            shared.batch_states.clear();
            shared.batch_scores.clear();
            const std::size_t capacity = batch_capacity();
            while (!shared.pending.empty() && shared.batch.size() < capacity) {
                const auto& candidate = shared.pending.back();
                if (candidate->service != service_) {
                    break;
                }
                std::shared_ptr<PendingRequest> taken = shared.pending.back();
                shared.pending.pop_back();
                shared.batch_states.push_back(taken->state);
                shared.batch_scores.push_back(0);
                shared.batch.push_back(std::move(taken));
            }
            std::shared_ptr<GpuNnueService> service = service_;
            lock.unlock();
            std::string error;
            const bool ok = service != nullptr &&
                service->evaluate(
                    std::span<const GameState*>(shared.batch_states.data(),
                                                shared.batch_states.size()),
                    std::span<std::int32_t>(shared.batch_scores.data(),
                                            shared.batch_scores.size()),
                    error);
            lock.lock();
            for (std::size_t index = 0; index < shared.batch.size(); ++index) {
                shared.batch[index]->done = true;
                shared.batch[index]->failed = !ok;
                shared.batch[index]->score = ok ? shared.batch_scores[index] : 0;
            }
            // Drop the request references as soon as their owners are notified;
            // the vectors keep their capacity for the next batch.
            shared.batch.clear();
            shared.batch_states.clear();
            shared.batch_scores.clear();
            shared.in_flight = false;
            shared.cv.notify_all();
            continue;
        }
        // Wake when this request completes or when the current batch finishes:
        // a waiter whose request was not in that batch must get a chance to
        // take over as the next leader, or its request could sleep forever.
        shared.cv.wait(lock, [active, &shared] {
            return active->done || !shared.in_flight;
        });
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
