#include "koi/gpu/gpu_nnue_service.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "koi/game_state.hpp"
#include "koi/gpu/cuda_driver.hpp"
#include "koi/gpu/ptx_variant.hpp"
#include "koi/nnue.hpp"

#if KOI_GPU_INFERENCE_AVAILABLE
#include "koi_nnue_v5_ptx.hpp"
#endif

namespace koi::gpu {

namespace {

constexpr std::size_t kInput = 36864;
constexpr std::size_t kBuckets = 8;
constexpr std::size_t kMinimumCapacity = kMaximumGpuBatchSize;

} // namespace

struct GpuNnueService::Impl {
#if KOI_GPU_INFERENCE_AVAILABLE
    CudaDriver driver;
    CUfunction kernel = nullptr;
    CUdeviceptr occupancy = 0;
    CUdeviceptr pieces = 0;
    CUdeviceptr side = 0;
    CUdeviceptr kings = 0;
    CUdeviceptr feature_weights = 0;
    CUdeviceptr hidden_bias = 0;
    CUdeviceptr l1_weights = 0;
    CUdeviceptr l1_bias = 0;
    CUdeviceptr output_weights = 0;
    CUdeviceptr output_bias = 0;
    CUdeviceptr scores = 0;
    CUdeviceptr overflow = 0;
    std::size_t capacity = 0;
    int l1_units = 0;
    int hidden_units = 0;
    int l1_shift = 0;
    int output_shift = 0;
    std::vector<std::uint64_t> host_occupancy;
    std::vector<std::uint8_t> host_pieces;
    std::vector<std::uint8_t> host_side;
    std::vector<std::uint8_t> host_kings;
    std::vector<std::int32_t> host_scores;
    std::vector<std::uint8_t> host_overflow;

    // Frees every device buffer.  Called from the service destructor so that a
    // failed create() and an EvalFile swap do not leak the uploaded network.
    void release_all() noexcept {
        for (CUdeviceptr* pointer : {&occupancy, &pieces, &side, &kings,
                                     &feature_weights, &hidden_bias, &l1_weights,
                                     &l1_bias, &output_weights, &output_bias,
                                     &scores, &overflow}) {
            if (*pointer != 0) {
                driver.release(*pointer);
                *pointer = 0;
            }
        }
        kernel = nullptr;
    }
#endif
};

GpuNnueService::GpuNnueService() : impl_(std::make_unique<Impl>()) {}

GpuNnueService::~GpuNnueService() {
#if KOI_GPU_INFERENCE_AVAILABLE
    if (impl_ != nullptr) {
        impl_->release_all();
    }
#endif
}
GpuNnueService::GpuNnueService(GpuNnueService&&) noexcept = default;
GpuNnueService& GpuNnueService::operator=(GpuNnueService&&) noexcept = default;

std::unique_ptr<GpuNnueService> GpuNnueService::create(const NnueNetwork& network,
                                                      std::string& error) {
#if !KOI_GPU_INFERENCE_AVAILABLE
    (void)network;
    error = "this build has no GPU NNUE kernel";
    return nullptr;
#else
    if (network.manifest.version != kKoiNnueHalfkaThreatV5FormatVersion ||
        network.manifest.layer_sizes[0] != kInput ||
        network.manifest.layer_sizes[2] != kBuckets) {
        error = "the GPU kernel only supports version 5 containers";
        return nullptr;
    }
    const std::size_t hidden = network.manifest.layer_sizes[1];
    const std::size_t l1 = network.manifest.layer_sizes[3];
    if (hidden != 1536U || l1 != 32U) {
        error = "the GPU kernel currently supports hidden 1536 and L1 32 only";
        return nullptr;
    }
    if (hidden == 0 || hidden % 16U != 0U || l1 < 8U || l1 > 128U ||
        network.feature_weights.size() != kInput * hidden ||
        network.hidden_bias.size() != hidden ||
        network.l1_weights.size() != l1 * hidden ||
        network.l1_bias.size() != l1 ||
        network.bottleneck_weights.size() != kBuckets * l1 ||
        network.bottleneck_bias.size() != kBuckets) {
        error = "the network tensors do not match the GPU kernel layout";
        return nullptr;
    }

    auto service = std::unique_ptr<GpuNnueService>(new GpuNnueService());
    Impl& impl = *service->impl_;
    impl.driver = CudaDriver::open(error);
    if (!impl.driver.available()) {
        return nullptr;
    }
    // Pick the newest embedded PTX module the device can run.  PTX is only
    // forward compatible, so a device older than the oldest module has no GPU
    // path; KOI_GPU_PTX_ARCH forces a specific module for diagnostics.  Loading
    // is also bounded by the installed driver, which may predate the PTX
    // version of the newest module, so the candidates are tried newest first
    // and the newest module that actually loads wins.
    const auto variants = std::span<const NnueV5PtxVariant>(
        kNnueV5PtxVariants, static_cast<std::size_t>(kNnueV5PtxVariantCount));
    std::vector<int> candidates(static_cast<std::size_t>(kNnueV5PtxVariantCount));
    std::size_t candidate_count = 0;
    if (const char* forced = std::getenv("KOI_GPU_PTX_ARCH");
        forced != nullptr && *forced != '\0') {
        const long wanted = std::strtol(forced, nullptr, 10);
        for (std::size_t index = 0; index < variants.size(); ++index) {
            if (variants[index].major * 10 + variants[index].minor == wanted) {
                candidates[0] = static_cast<int>(index);
                candidate_count = 1;
                break;
            }
        }
        if (candidate_count == 0) {
            error = "KOI_GPU_PTX_ARCH names no embedded PTX variant";
            return nullptr;
        }
    } else {
        const CudaDeviceInfo& info = impl.driver.device_info();
        candidate_count = select_ptx_candidates(variants, info.compute_major,
                                                info.compute_minor, candidates);
        if (candidate_count == 0) {
            error = "no embedded PTX variant supports sm_" +
                std::to_string(info.compute_major) + std::to_string(info.compute_minor) +
                " (the oldest embedded variant is sm_61)";
            return nullptr;
        }
    }
    bool module_loaded = false;
    std::string load_error;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        const NnueV5PtxVariant& chosen =
            variants[static_cast<std::size_t>(candidates[index])];
        const auto ptx = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(chosen.source),
            std::char_traits<char>::length(chosen.source));
        if (impl.driver.load_module(ptx, load_error) &&
            impl.driver.module_function("koi_nnue_v5_eval", impl.kernel, load_error)) {
            module_loaded = true;
            break;
        }
    }
    if (!module_loaded) {
        error = "no embedded PTX variant could be loaded: " + load_error;
        return nullptr;
    }

    impl.hidden_units = static_cast<int>(hidden);
    impl.l1_units = static_cast<int>(l1);
    impl.l1_shift = network.l1_shift;
    impl.output_shift = network.output_shift;
    impl.capacity = kMinimumCapacity;
    const std::size_t capacity = impl.capacity;

    const auto upload = [&](const void* source, std::size_t bytes, CUdeviceptr& pointer) {
        return impl.driver.allocate(bytes, pointer, error) &&
            impl.driver.upload(source, pointer, bytes, error);
    };
    if (!upload(network.feature_weights.data(),
                network.feature_weights.size() * sizeof(std::int16_t), impl.feature_weights) ||
        !upload(network.hidden_bias.data(),
                network.hidden_bias.size() * sizeof(std::int32_t), impl.hidden_bias) ||
        !upload(network.l1_weights.data(),
                network.l1_weights.size() * sizeof(std::int8_t), impl.l1_weights) ||
        !upload(network.l1_bias.data(),
                network.l1_bias.size() * sizeof(std::int32_t), impl.l1_bias) ||
        !upload(network.bottleneck_weights.data(),
                network.bottleneck_weights.size() * sizeof(std::int8_t), impl.output_weights) ||
        !upload(network.bottleneck_bias.data(),
                network.bottleneck_bias.size() * sizeof(std::int32_t), impl.output_bias) ||
        !impl.driver.allocate(capacity * 2 * sizeof(std::uint64_t), impl.occupancy, error) ||
        !impl.driver.allocate(capacity * 64 * sizeof(std::uint8_t), impl.pieces, error) ||
        !impl.driver.allocate(capacity * sizeof(std::uint8_t), impl.side, error) ||
        !impl.driver.allocate(capacity * 2 * sizeof(std::uint8_t), impl.kings, error) ||
        !impl.driver.allocate(capacity * sizeof(std::int32_t), impl.scores, error) ||
        !impl.driver.allocate(capacity * sizeof(std::uint8_t), impl.overflow, error)) {
        return nullptr;
    }
    impl.host_occupancy.resize(capacity * 2);
    impl.host_pieces.resize(capacity * 64);
    impl.host_side.resize(capacity);
    impl.host_kings.resize(capacity * 2);
    impl.host_scores.resize(capacity);
    impl.host_overflow.resize(capacity);
    return service;
#endif
}

bool GpuNnueService::available() const noexcept {
#if KOI_GPU_INFERENCE_AVAILABLE
    return impl_ != nullptr && impl_->driver.available() && impl_->kernel != nullptr;
#else
    return false;
#endif
}

bool GpuNnueService::evaluate(std::span<const GameState*> states,
                              std::span<std::int32_t> scores, std::string& error) {
#if !KOI_GPU_INFERENCE_AVAILABLE
    (void)states;
    (void)scores;
    error = "this build has no GPU NNUE kernel";
    return false;
#else
    Impl& impl = *impl_;
    if (states.empty()) {
        return true;
    }
    if (scores.size() != states.size()) {
        error = "the score buffer must match the batch size";
        return false;
    }
    if (states.size() > impl.capacity) {
        error = "the batch exceeds the GPU staging capacity";
        return false;
    }
    for (std::size_t index = 0; index < states.size(); ++index) {
        const GameState& state = *states[index];
        const PositionFeatures& features = state.position_features();
        std::uint64_t* occupancy = impl.host_occupancy.data() + index * 2;
        std::uint8_t* pieces = impl.host_pieces.data() + index * 64;
        occupancy[0] = 0;
        occupancy[1] = 0;
        for (std::size_t square = 0; square < 64; ++square) {
            const Piece piece = features.board[square];
            if (piece.empty()) {
                pieces[square] = 0;
                continue;
            }
            const std::uint8_t color = piece.color == Color::white ? 0U : 1U;
            pieces[square] = static_cast<std::uint8_t>(
                static_cast<std::uint8_t>(piece.type) | (color << 3));
            occupancy[color] |= (std::uint64_t{1} << square);
        }
        impl.host_side[index] = features.side_to_move == Color::white ? 0U : 1U;
        impl.host_kings[index * 2] =
            static_cast<std::uint8_t>(features.king_squares[0].index());
        impl.host_kings[index * 2 + 1] =
            static_cast<std::uint8_t>(features.king_squares[1].index());
    }

    const std::size_t count = states.size();
    const auto fail = [&error](const char* step) {
        error = std::string(step) + " failed: " + (error.empty() ? "unknown error" : error);
        return false;
    };
    if (!impl.driver.upload(impl.host_occupancy.data(), impl.occupancy,
                            count * 2 * sizeof(std::uint64_t), error) ||
        !impl.driver.upload(impl.host_pieces.data(), impl.pieces,
                            count * 64 * sizeof(std::uint8_t), error) ||
        !impl.driver.upload(impl.host_side.data(), impl.side,
                            count * sizeof(std::uint8_t), error) ||
        !impl.driver.upload(impl.host_kings.data(), impl.kings,
                            count * 2 * sizeof(std::uint8_t), error)) {
        return fail("upload");
    }
    void* arguments[] = {
        &impl.occupancy, &impl.pieces, &impl.side, &impl.kings,
        &impl.feature_weights, &impl.hidden_bias, &impl.l1_weights, &impl.l1_bias,
        &impl.output_weights, &impl.output_bias, &impl.l1_shift, &impl.output_shift,
        &impl.scores, &impl.overflow,
    };
    if (!impl.driver.launch_and_wait(impl.kernel, static_cast<unsigned>(count), 256, 0,
                                     arguments, error)) {
        return fail("launch");
    }
    if (!impl.driver.download(impl.scores, impl.host_scores.data(),
                              count * sizeof(std::int32_t), error) ||
        !impl.driver.download(impl.overflow, impl.host_overflow.data(),
                              count * sizeof(std::uint8_t), error)) {
        return fail("download");
    }
    if (std::any_of(impl.host_overflow.begin(), impl.host_overflow.begin() +
                        static_cast<std::ptrdiff_t>(count),
                    [](std::uint8_t flag) { return flag != 0; })) {
        error = "the GPU kernel exceeded its feature capacity";
        return false;
    }
    std::copy_n(impl.host_scores.begin(), static_cast<std::ptrdiff_t>(count),
                scores.begin());
    return true;
#endif
}

} // namespace koi::gpu
