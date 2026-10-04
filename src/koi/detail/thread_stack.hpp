#pragma once

// Engine worker threads need a large stack for deep tactical recursion. Linux
// raises the process-wide glibc default to 16 MiB in src/main.cpp and the
// Windows build links a 16 MiB stack reserve that worker threads inherit, but
// Darwin has no process-wide equivalent (Apple's pthread.h declares neither
// pthread_setattr_default_np nor any other default-stack setter), so a Darwin
// pthread keeps libpthread's 512 KiB default. `spawn_worker_thread` gives each
// Darwin worker an explicit, documented 64 MiB stack; the size is a virtual
// reservation, so an idle worker does not pay for it in resident memory. Every
// other platform keeps constructing a plain std::thread.

#include <cstddef>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <exception>
#include <memory>
#include <system_error>
#include <type_traits>

#include <pthread.h>
#endif

namespace koi::detail {

// Chosen worker stack size on Darwin: larger than the Linux/Windows 16 MiB
// worker budget so deep lines keep a safety margin, and far above the 512 KiB
// pthread default it replaces.
inline constexpr std::size_t kWorkerStackBytes = std::size_t{64} * 1024 * 1024;

#if defined(__APPLE__)

class WorkerThread;

// Starts `callable` on a pthread whose stack is `stack_bytes`, returning a
// move-only handle that mirrors std::thread's join/detach/joinable/get_id
// surface. Throws std::system_error like std::thread when the thread cannot be
// created.
template <typename Callable>
[[nodiscard]] WorkerThread spawn_worker_thread(
    Callable&& callable, std::size_t stack_bytes = kWorkerStackBytes);

// Move-only pthread handle. A moved-from handle is not joinable; destroying a
// joinable handle or assigning over one calls std::terminate, exactly like
// std::thread.
class WorkerThread {
public:
    WorkerThread() noexcept = default;

    WorkerThread(WorkerThread&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    WorkerThread& operator=(WorkerThread&& other) noexcept {
        if (this != &other) {
            if (joinable()) {
                std::terminate();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    WorkerThread(const WorkerThread&) = delete;
    WorkerThread& operator=(const WorkerThread&) = delete;

    ~WorkerThread() {
        if (joinable()) {
            std::terminate();
        }
    }

    [[nodiscard]] bool joinable() const noexcept { return handle_ != nullptr; }

    void join() {
        if (!joinable()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "WorkerThread::join");
        }
        if (pthread_equal(pthread_self(), handle_) != 0) {
            throw std::system_error(
                std::make_error_code(std::errc::resource_deadlock_would_occur),
                "WorkerThread::join");
        }
        const int result = pthread_join(handle_, nullptr);
        if (result != 0) {
            throw std::system_error(result, std::system_category(), "WorkerThread::join");
        }
        handle_ = nullptr;
    }

    void detach() {
        if (!joinable()) {
            throw std::system_error(std::make_error_code(std::errc::invalid_argument),
                                    "WorkerThread::detach");
        }
        const int result = pthread_detach(handle_);
        if (result != 0) {
            throw std::system_error(result, std::system_category(), "WorkerThread::detach");
        }
        handle_ = nullptr;
    }

    // The engine only compares this id against the calling thread
    // (SearchSession::wait guards against joining itself). Returning the
    // calling thread's id when the caller is this worker preserves that guard;
    // every other caller receives the default-constructed id, which compares
    // unequal to any live thread.
    [[nodiscard]] std::thread::id get_id() const noexcept {
        if (joinable() && pthread_equal(pthread_self(), handle_) != 0) {
            return std::this_thread::get_id();
        }
        return std::thread::id{};
    }

private:
    explicit WorkerThread(pthread_t handle) noexcept : handle_(handle) {}

    template <typename Callable>
    friend WorkerThread spawn_worker_thread(Callable&& callable, std::size_t stack_bytes);

    pthread_t handle_ = nullptr;
};

namespace worker_thread_detail {

template <typename State>
void* trampoline(void* raw) {
    // The pthread owns the heap callable until it exits, so detach() can
    // outlive the WorkerThread handle safely.
    std::unique_ptr<State> state(static_cast<State*>(raw));
    try {
        (*state)();
    } catch (...) {
        // A C++ exception cannot cross the pthread entry point. std::thread
        // terminates on an escaping exception, so match that contract.
        std::terminate();
    }
    return nullptr;
}

} // namespace worker_thread_detail

template <typename Callable>
[[nodiscard]] WorkerThread spawn_worker_thread(
    Callable&& callable, const std::size_t stack_bytes) {
    using State = std::decay_t<Callable>;
    auto state = std::make_unique<State>(std::forward<Callable>(callable));

    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0) {
        throw std::system_error(result, std::system_category(), "pthread_attr_init");
    }
    result = pthread_attr_setstacksize(&attributes, stack_bytes);
    if (result != 0) {
        pthread_attr_destroy(&attributes);
        throw std::system_error(result, std::system_category(), "pthread_attr_setstacksize");
    }

    pthread_t handle = nullptr;
    State* const raw = state.release();
    result = pthread_create(&handle, &attributes, &worker_thread_detail::trampoline<State>, raw);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        delete raw;
        throw std::system_error(result, std::system_category(), "pthread_create");
    }
    return WorkerThread(handle);
}

#else

// Every other platform already gives worker threads the platform stack budget
// (glibc default raised in src/main.cpp, Windows PE stack reserve), so the
// helper is exactly std::thread and the parameter is accepted for a uniform
// call shape.
using WorkerThread = std::thread;

template <typename Callable>
[[nodiscard]] WorkerThread spawn_worker_thread(
    Callable&& callable, const std::size_t /*stack_bytes*/ = kWorkerStackBytes) {
    return WorkerThread(std::forward<Callable>(callable));
}

#endif

} // namespace koi::detail
