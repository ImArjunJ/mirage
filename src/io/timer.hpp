#pragma once

#include <chrono>
#include <coroutine>

#include "io/event_loop.hpp"

namespace mirage::io {

class steady_timer {
public:
    explicit steady_timer(io_context& ctx) : ctx_(&ctx) {}

    void expires_after(std::chrono::steady_clock::duration d) {
        deadline_ = std::chrono::steady_clock::now() + d;
    }

    void expires_at(std::chrono::steady_clock::time_point tp) { deadline_ = tp; }

    [[nodiscard]] task<void> async_wait() { co_await awaiter{ctx_, deadline_}; }

private:
    struct awaiter {
        io_context* ctx;
        std::chrono::steady_clock::time_point deadline;

        [[nodiscard]] bool await_ready() const noexcept {
            return deadline <= std::chrono::steady_clock::now();
        }

        void await_suspend(std::coroutine_handle<> h) const {
            static_cast<void>(ctx->add_timer(deadline, h));
        }

        void await_resume() noexcept {}
    };

    io_context* ctx_;
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace mirage::io
