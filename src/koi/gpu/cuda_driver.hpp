#pragma once

// Minimal dynamic loader for the CUDA driver API.  Koi links no CUDA library:
// `nvcuda.dll` on Windows and `libcuda.so.1` on Linux are resolved at runtime,
// so a machine without a CUDA driver simply reports the GPU as unavailable and
// every caller falls back to the CPU path.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Opaque driver types, declared locally so this header does not need cuda.h.
using CUresult = int;
using CUdevice = int;
using CUcontext = struct CUctx_st*;
using CUmodule = struct CUmod_st*;
using CUfunction = struct CUfunc_st*;
using CUstream = struct CUstream_st*;
using CUdeviceptr = unsigned long long;
using CUjit_option = int;

namespace koi::gpu {

inline constexpr int kCudaSuccess = 0;

struct CudaDeviceInfo {
    std::string name;
    int compute_major = 0;
    int compute_minor = 0;
    int driver_version = 0;
};

// One process-wide driver handle: loads the library, initializes the driver,
// opens the primary device, and owns a context and a stream.
class CudaDriver {
public:
    CudaDriver() = default;
    ~CudaDriver();

    CudaDriver(const CudaDriver&) = delete;
    CudaDriver& operator=(const CudaDriver&) = delete;
    CudaDriver(CudaDriver&& other) noexcept;
    CudaDriver& operator=(CudaDriver&& other) noexcept;

    // Loads the CUDA driver library, initializes the API, selects device 0, and
    // creates a context plus a stream.  On failure the returned object reports
    // `available() == false` and `error` carries the reason.
    [[nodiscard]] static CudaDriver open(std::string& error);

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] const CudaDeviceInfo& device_info() const noexcept { return info_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return error_; }

    [[nodiscard]] bool load_module(std::span<const std::uint8_t> ptx,
                                   std::string& error);
    [[nodiscard]] bool module_function(std::string_view name,
                                       CUfunction& function,
                                       std::string& error) const;
    void unload_module() noexcept;

    [[nodiscard]] bool allocate(std::size_t bytes, CUdeviceptr& pointer,
                                std::string& error);
    void release(CUdeviceptr pointer) noexcept;
    [[nodiscard]] bool upload(const void* source, CUdeviceptr destination,
                              std::size_t bytes, std::string& error) const;
    [[nodiscard]] bool download(CUdeviceptr source, void* destination,
                                std::size_t bytes, std::string& error) const;
    [[nodiscard]] bool host_allocate(std::size_t bytes, void*& pointer,
                                     std::string& error);
    void host_release(void* pointer) noexcept;

    // Launches a kernel with a flat argument list and blocks until it finishes.
    [[nodiscard]] bool launch_and_wait(CUfunction function, unsigned grid_x,
                                       unsigned block_x, unsigned shared_bytes,
                                       void** arguments, std::string& error);
    [[nodiscard]] bool synchronize(std::string& error);

private:
    struct Api;
    [[nodiscard]] bool check(CUresult result, std::string_view what,
                             std::string& error) const;
    // Makes this driver's context current on the calling thread; CUDA contexts
    // are per-thread, so evaluation workers must call this before device work.
    [[nodiscard]] bool ensure_context(std::string& error) const;
    // Releases the module, stream, context, and library handle.  Called by the
    // destructor and by move assignment.
    void release() noexcept;

    Api* api_ = nullptr;
    CUcontext context_ = nullptr;
    CUstream stream_ = nullptr;
    CUmodule module_ = nullptr;
    CudaDeviceInfo info_{};
    std::string error_;
    bool available_ = false;
};

} // namespace koi::gpu
