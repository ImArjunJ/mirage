#pragma once

#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "io/platform.hpp"
#include "io/task.hpp"

namespace mirage::io {

class io_context {
public:
    io_context();
    ~io_context();
    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    void run();
    void run_for(std::chrono::milliseconds ms);
    void stop();

#ifdef _WIN32
    void associate(socket_t s);
    void post_completion(std::coroutine_handle<> h);
#else
    void watch_read(int fd, std::coroutine_handle<> h);
    void watch_write(int fd, std::coroutine_handle<> h);
    void cancel(int fd);
#endif

    [[nodiscard]] uint64_t add_timer(std::chrono::steady_clock::time_point deadline,
                                     std::coroutine_handle<> h);
    void cancel_timer(uint64_t id);

    void schedule(std::coroutine_handle<> h);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

namespace detail {

struct detached_task {
    struct promise_type {
        [[nodiscard]] std::suspend_never initial_suspend() noexcept { return {}; }

        [[nodiscard]] auto final_suspend() noexcept {
            struct destroyer {
                [[nodiscard]] bool await_ready() noexcept { return false; }
                void await_suspend(std::coroutine_handle<> h) noexcept { h.destroy(); }
                void await_resume() noexcept {}
            };
            return destroyer{};
        }

        detached_task get_return_object() { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };
};

}  // namespace detail

inline void co_spawn(io_context& /*ctx*/, task<void> t) {
    [](task<void> inner) -> detail::detached_task { co_await std::move(inner); }(std::move(t));
}

template <typename F>
    requires std::invocable<F> && std::same_as<std::invoke_result_t<F>, task<void>>
void co_spawn(io_context& /*ctx*/, F f) {
    [](F func) -> detail::detached_task { co_await func(); }(std::move(f));
}

}  // namespace mirage::io
