#include "koi/gpu/cuda_driver.hpp"

#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace koi::gpu {
namespace {

#if defined(_WIN32)
using DriverLibrary = HMODULE;
#else
using DriverLibrary = void*;
#endif

template <typename T>
void resolve(DriverLibrary library, const char* name, T& target) {
#if defined(_WIN32)
    target = reinterpret_cast<T>(GetProcAddress(library, name));
#elif defined(__linux__)
    target = reinterpret_cast<T>(dlsym(library, name));
#else
    (void)library;
    (void)name;
    target = nullptr;
#endif
}

} // namespace

struct CudaDriver::Api {
    DriverLibrary library = nullptr;
    CUresult (*init)(unsigned) = nullptr;
    CUresult (*driver_get_version)(int*) = nullptr;
    CUresult (*device_get_count)(int*) = nullptr;
    CUresult (*device_get)(CUdevice*, int) = nullptr;
    CUresult (*device_get_name)(char*, int, CUdevice) = nullptr;
    CUresult (*device_compute_capability)(int*, int*, CUdevice) = nullptr;
    CUresult (*ctx_create)(CUcontext*, unsigned, CUdevice) = nullptr;
    CUresult (*ctx_destroy)(CUcontext) = nullptr;
    CUresult (*ctx_set_current)(CUcontext) = nullptr;
    CUresult (*module_load_data_ex)(CUmodule*, const void*, unsigned,
                                    CUjit_option*, void**) = nullptr;
    CUresult (*module_unload)(CUmodule) = nullptr;
    CUresult (*module_get_function)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult (*mem_alloc)(CUdeviceptr*, std::size_t) = nullptr;
    CUresult (*mem_free)(CUdeviceptr) = nullptr;
    CUresult (*memcpy_htod)(CUdeviceptr, const void*, std::size_t) = nullptr;
    CUresult (*memcpy_dtoh)(void*, CUdeviceptr, std::size_t) = nullptr;
    CUresult (*stream_create)(CUstream*, unsigned) = nullptr;
    CUresult (*stream_destroy)(CUstream) = nullptr;
    CUresult (*stream_synchronize)(CUstream) = nullptr;
    CUresult (*launch_kernel)(CUfunction, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned, CUstream, void**,
                              void**) = nullptr;
    CUresult (*get_error_string)(CUresult, const char**) = nullptr;
    CUresult (*mem_host_alloc)(void**, std::size_t, unsigned) = nullptr;
    CUresult (*mem_free_host)(void*) = nullptr;
};

CudaDriver::~CudaDriver() { release(); }

void CudaDriver::release() noexcept {
    unload_module();
    if (stream_ != nullptr && api_ != nullptr && api_->stream_destroy != nullptr) {
        api_->stream_destroy(stream_);
        stream_ = nullptr;
    }
    if (context_ != nullptr && api_ != nullptr && api_->ctx_destroy != nullptr) {
        api_->ctx_destroy(context_);
        context_ = nullptr;
    }
    if (api_ != nullptr && api_->library != nullptr) {
#if defined(_WIN32)
        FreeLibrary(api_->library);
#elif defined(__linux__)
        dlclose(api_->library);
#endif
        api_->library = nullptr;
    }
    delete api_;
    api_ = nullptr;
}

CudaDriver::CudaDriver(CudaDriver&& other) noexcept
    : api_(std::exchange(other.api_, nullptr)),
      context_(std::exchange(other.context_, nullptr)),
      stream_(std::exchange(other.stream_, nullptr)),
      module_(std::exchange(other.module_, nullptr)),
      info_(std::move(other.info_)),
      error_(std::move(other.error_)),
      available_(std::exchange(other.available_, false)) {}

CudaDriver& CudaDriver::operator=(CudaDriver&& other) noexcept {
    if (this != &other) {
        release();
        api_ = std::exchange(other.api_, nullptr);
        context_ = std::exchange(other.context_, nullptr);
        stream_ = std::exchange(other.stream_, nullptr);
        module_ = std::exchange(other.module_, nullptr);
        info_ = std::move(other.info_);
        error_ = std::move(other.error_);
        available_ = std::exchange(other.available_, false);
    }
    return *this;
}

bool CudaDriver::check(CUresult result, std::string_view what,
                       std::string& error) const {
    if (result == kCudaSuccess) {
        return true;
    }
    const char* text = "unknown CUDA error";
    if (api_ != nullptr && api_->get_error_string != nullptr) {
        api_->get_error_string(result, &text);
    }
    error.assign(what);
    error.append(": ");
    error.append(text);
    error.append(" (");
    error.append(std::to_string(result));
    error.append(")");
    return false;
}

bool CudaDriver::ensure_context(std::string& error) const {
    // CUDA contexts are per-thread: a worker that did not create the context
    // must make it current before any call that touches device memory.
    if (context_ == nullptr || api_ == nullptr || api_->ctx_set_current == nullptr) {
        return true;
    }
    return check(api_->ctx_set_current(context_), "cuCtxSetCurrent", error);
}

CudaDriver CudaDriver::open(std::string& error) {
    CudaDriver driver;
    driver.api_ = new Api();
#if defined(_WIN32)
    driver.api_->library = LoadLibraryW(L"nvcuda.dll");
    if (driver.api_->library == nullptr) {
        error = "nvcuda.dll is not available";
        driver.error_ = error;
        return driver;
    }
#elif defined(__linux__)
    (void)dlerror(); // clear any stale loader error before probing
    driver.api_->library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (driver.api_->library == nullptr) {
        error = "libcuda.so.1 is not available";
        if (const char* detail = dlerror(); detail != nullptr) {
            error.append(": ");
            error.append(detail);
        }
        driver.error_ = error;
        return driver;
    }
#else
    error = "CUDA is not wired for this platform";
    driver.error_ = error;
    return driver;
#endif
    DriverLibrary library = driver.api_->library;
    resolve(library, "cuInit", driver.api_->init);
    resolve(library, "cuDriverGetVersion", driver.api_->driver_get_version);
    resolve(library, "cuDeviceGetCount", driver.api_->device_get_count);
    resolve(library, "cuDeviceGet", driver.api_->device_get);
    resolve(library, "cuDeviceGetName", driver.api_->device_get_name);
    resolve(library, "cuDeviceComputeCapability",
            driver.api_->device_compute_capability);
    resolve(library, "cuCtxCreate_v2", driver.api_->ctx_create);
    resolve(library, "cuCtxDestroy_v2", driver.api_->ctx_destroy);
    resolve(library, "cuCtxSetCurrent", driver.api_->ctx_set_current);
    resolve(library, "cuModuleLoadDataEx", driver.api_->module_load_data_ex);
    resolve(library, "cuModuleUnload", driver.api_->module_unload);
    resolve(library, "cuModuleGetFunction", driver.api_->module_get_function);
    resolve(library, "cuMemAlloc_v2", driver.api_->mem_alloc);
    resolve(library, "cuMemFree_v2", driver.api_->mem_free);
    resolve(library, "cuMemcpyHtoD_v2", driver.api_->memcpy_htod);
    resolve(library, "cuMemcpyDtoH_v2", driver.api_->memcpy_dtoh);
    resolve(library, "cuStreamCreate", driver.api_->stream_create);
    resolve(library, "cuStreamDestroy_v2", driver.api_->stream_destroy);
    resolve(library, "cuStreamSynchronize", driver.api_->stream_synchronize);
    resolve(library, "cuLaunchKernel", driver.api_->launch_kernel);
    resolve(library, "cuGetErrorString", driver.api_->get_error_string);
    resolve(library, "cuMemHostAlloc", driver.api_->mem_host_alloc);
    resolve(library, "cuMemFreeHost", driver.api_->mem_free_host);

    if (driver.api_->init == nullptr || driver.api_->ctx_create == nullptr ||
        driver.api_->launch_kernel == nullptr) {
        error = "the CUDA driver library does not expose the required entry points";
        driver.error_ = error;
        return driver;
    }
    if (!driver.check(driver.api_->init(0), "cuInit", error)) {
        driver.error_ = error;
        return driver;
    }
    int count = 0;
    if (driver.api_->device_get_count == nullptr ||
        !driver.check(driver.api_->device_get_count(&count), "cuDeviceGetCount",
                      error)) {
        driver.error_ = error;
        return driver;
    }
    if (count <= 0) {
        error = "no CUDA device is present";
        driver.error_ = error;
        return driver;
    }
    CUdevice device = 0;
    if (!driver.check(driver.api_->device_get(&device, 0), "cuDeviceGet", error)) {
        driver.error_ = error;
        return driver;
    }
    char name[256] = {};
    if (driver.api_->device_get_name != nullptr &&
        driver.check(driver.api_->device_get_name(name, sizeof(name), device),
                     "cuDeviceGetName", error)) {
        driver.info_.name = name;
    }
    if (driver.api_->device_compute_capability != nullptr) {
        int major = 0;
        int minor = 0;
        (void)driver.check(driver.api_->device_compute_capability(&major, &minor, device),
                           "cuDeviceComputeCapability", error);
        driver.info_.compute_major = major;
        driver.info_.compute_minor = minor;
    }
    if (driver.api_->driver_get_version != nullptr) {
        int version = 0;
        if (driver.check(driver.api_->driver_get_version(&version),
                         "cuDriverGetVersion", error)) {
            driver.info_.driver_version = version;
        }
    }
    CUcontext context = nullptr;
    if (!driver.check(driver.api_->ctx_create(&context, 0, device), "cuCtxCreate",
                      error)) {
        driver.error_ = error;
        return driver;
    }
    driver.context_ = context;
    if (driver.api_->stream_create != nullptr) {
        CUstream stream = nullptr;
        if (!driver.check(driver.api_->stream_create(&stream, 0),
                          "cuStreamCreate", error)) {
            driver.error_ = error;
            return driver;
        }
        driver.stream_ = stream;
    }
    driver.available_ = true;
    error.clear();
    return driver;
}

bool CudaDriver::load_module(std::span<const std::uint8_t> ptx,
                             std::string& error) {
    if (!ensure_context(error)) {
        return false;
    }
    if (!available_ || api_->module_load_data_ex == nullptr) {
        error = "CUDA driver is not available";
        return false;
    }
    unload_module();
    CUmodule module = nullptr;
    if (!check(api_->module_load_data_ex(&module, ptx.data(), 0, nullptr, nullptr),
               "cuModuleLoadDataEx", error)) {
        return false;
    }
    module_ = module;
    return true;
}

bool CudaDriver::module_function(std::string_view name, CUfunction& function,
                                 std::string& error) const {
    if (module_ == nullptr || api_->module_get_function == nullptr) {
        error = "no CUDA module is loaded";
        return false;
    }
    if (!ensure_context(error)) {
        return false;
    }
    const std::string text(name);
    CUfunction resolved = nullptr;
    if (!check(api_->module_get_function(&resolved, module_, text.c_str()),
               "cuModuleGetFunction", error)) {
        return false;
    }
    function = resolved;
    return true;
}

void CudaDriver::unload_module() noexcept {
    if (module_ != nullptr && api_ != nullptr && api_->module_unload != nullptr) {
        api_->module_unload(module_);
    }
    module_ = nullptr;
}

bool CudaDriver::allocate(std::size_t bytes, CUdeviceptr& pointer,
                          std::string& error) {
    if (!available_ || api_->mem_alloc == nullptr) {
        error = "CUDA driver is not available";
        return false;
    }
    if (!ensure_context(error)) {
        return false;
    }
    CUdeviceptr result = 0;
    if (!check(api_->mem_alloc(&result, bytes), "cuMemAlloc", error)) {
        return false;
    }
    pointer = result;
    return true;
}

void CudaDriver::release(CUdeviceptr pointer) noexcept {
    if (pointer != 0 && api_ != nullptr && api_->mem_free != nullptr) {
        api_->mem_free(pointer);
    }
}

bool CudaDriver::upload(const void* source, CUdeviceptr destination,
                        std::size_t bytes, std::string& error) const {
    if (!ensure_context(error)) {
        return false;
    }
    return check(api_->memcpy_htod(destination, source, bytes), "cuMemcpyHtoD",
                 error);
}

bool CudaDriver::download(CUdeviceptr source, void* destination, std::size_t bytes,
                          std::string& error) const {
    if (!ensure_context(error)) {
        return false;
    }
    return check(api_->memcpy_dtoh(destination, source, bytes), "cuMemcpyDtoH",
                 error);
}

bool CudaDriver::host_allocate(std::size_t bytes, void*& pointer,
                               std::string& error) {
    if (api_->mem_host_alloc == nullptr) {
        // Fall back to pageable memory; transfers stay correct, just slower.
        pointer = ::operator new(bytes);
        return true;
    }
    void* result = nullptr;
    if (!check(api_->mem_host_alloc(&result, bytes, 0), "cuMemHostAlloc", error)) {
        return false;
    }
    pointer = result;
    return true;
}

void CudaDriver::host_release(void* pointer) noexcept {
    if (pointer == nullptr) {
        return;
    }
    if (api_ != nullptr && api_->mem_host_alloc != nullptr &&
        api_->mem_free_host != nullptr) {
        api_->mem_free_host(pointer);
    } else {
        ::operator delete(pointer);
    }
}

bool CudaDriver::launch_and_wait(CUfunction function, unsigned grid_x,
                                 unsigned block_x, unsigned shared_bytes,
                                 void** arguments, std::string& error) {
    if (!available_) {
        error = "CUDA driver is not available";
        return false;
    }
    if (!ensure_context(error)) {
        return false;
    }
    if (!check(api_->launch_kernel(function, grid_x, 1, 1, block_x, 1, 1,
                                   shared_bytes, stream_, arguments, nullptr),
               "cuLaunchKernel", error)) {
        return false;
    }
    return synchronize(error);
}

bool CudaDriver::synchronize(std::string& error) {
    if (stream_ != nullptr && api_->stream_synchronize != nullptr) {
        if (!ensure_context(error)) {
            return false;
        }
        return check(api_->stream_synchronize(stream_), "cuStreamSynchronize",
                     error);
    }
    return true;
}

} // namespace koi::gpu
