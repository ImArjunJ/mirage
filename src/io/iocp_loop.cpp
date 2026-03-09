#ifdef _WIN32

#include <algorithm>
#include <coroutine>
#include <vector>

#include <mswsock.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "io/event_loop.hpp"

namespace mirage::io {

struct io_operation {
    OVERLAPPED overlapped{};
    std::coroutine_handle<> handle;
};

struct timer_entry {
    uint64_t id;
    std::chrono::steady_clock::time_point deadline;
    std::coroutine_handle<> handle;
};

struct io_context::impl {
    HANDLE iocp;
    bool running = false;
    std::vector<std::coroutine_handle<>> ready_queue;
    std::vector<timer_entry> timers;
    uint64_t next_timer_id = 1;

    impl() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    }

    ~impl() {
        CloseHandle(iocp);
        WSACleanup();
    }

    void fire_expired_timers() {
        auto now = std::chrono::steady_clock::now();
        auto it = timers.begin();
        while (it != timers.end()) {
            if (it->deadline <= now) {
                ready_queue.push_back(it->handle);
                it = timers.erase(it);
            } else {
                break;
            }
        }
    }

    int nearest_timer_ms(int requested_ms) {
        if (timers.empty()) {
            return requested_ms;
        }
        auto now = std::chrono::steady_clock::now();
        auto diff =
            std::chrono::duration_cast<std::chrono::milliseconds>(timers.front().deadline - now);
        int timer_ms = static_cast<int>(std::max(diff.count(), std::chrono::milliseconds::rep{0}));
        return std::min(requested_ms, timer_ms);
    }

    void poll(int timeout_ms) {
        fire_expired_timers();

        if (!ready_queue.empty()) {
            timeout_ms = 0;
        } else {
            timeout_ms = nearest_timer_ms(timeout_ms);
        }

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        LPOVERLAPPED overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(iocp, &bytes, &key, &overlapped,
                                            static_cast<DWORD>(timeout_ms));

        if (overlapped != nullptr) {
            auto* op = reinterpret_cast<io_operation*>(reinterpret_cast<char*>(overlapped) -
                                                       offsetof(io_operation, overlapped));
            if (op->handle) {
                ready_queue.push_back(op->handle);
            }
        } else if (ok && key != 0) {
            auto h = std::coroutine_handle<>::from_address(reinterpret_cast<void*>(key));
            if (h) {
                ready_queue.push_back(h);
            }
        }

        fire_expired_timers();

        std::vector<std::coroutine_handle<>> batch;
        batch.swap(ready_queue);
        for (auto h : batch) {
            h.resume();
        }
    }
};

io_context::io_context() : impl_(std::make_unique<impl>()) {}

io_context::~io_context() = default;

void io_context::run() {
    impl_->running = true;
    while (impl_->running) {
        impl_->poll(100);
    }
}

void io_context::run_for(std::chrono::milliseconds ms) {
    auto deadline = std::chrono::steady_clock::now() + ms;
    impl_->running = true;
    while (impl_->running) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        impl_->poll(static_cast<int>(
            std::min(remaining.count(), static_cast<std::chrono::milliseconds::rep>(100))));
    }
}

void io_context::stop() {
    impl_->running = false;
}

void io_context::associate(socket_t s) {
    CreateIoCompletionPort(reinterpret_cast<HANDLE>(s), impl_->iocp, 0, 0);
}

void io_context::post_completion(std::coroutine_handle<> h) {
    PostQueuedCompletionStatus(impl_->iocp, 0, reinterpret_cast<ULONG_PTR>(h.address()), nullptr);
}

uint64_t io_context::add_timer(std::chrono::steady_clock::time_point deadline,
                               std::coroutine_handle<> h) {
    uint64_t id = impl_->next_timer_id++;
    timer_entry entry{id, deadline, h};
    auto pos =
        std::ranges::lower_bound(impl_->timers, deadline, std::less<>{}, &timer_entry::deadline);
    impl_->timers.insert(pos, entry);
    return id;
}

void io_context::cancel_timer(uint64_t id) {
    auto it = std::ranges::find(impl_->timers, id, &timer_entry::id);
    if (it != impl_->timers.end()) {
        impl_->timers.erase(it);
    }
}

void io_context::schedule(std::coroutine_handle<> h) {
    PostQueuedCompletionStatus(impl_->iocp, 0, reinterpret_cast<ULONG_PTR>(h.address()), nullptr);
}

}  // namespace mirage::io

#endif
