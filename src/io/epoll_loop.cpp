#ifdef __linux__

#include <algorithm>
#include <coroutine>
#include <unordered_map>
#include <vector>

#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "io/event_loop.hpp"

namespace mirage::io {

struct fd_state {
    std::coroutine_handle<> read_handle;
    std::coroutine_handle<> write_handle;
};

struct timer_entry {
    uint64_t id;
    int timerfd;
    std::coroutine_handle<> handle;
};

struct io_context::impl {
    int epoll_fd;
    std::unordered_map<int, fd_state> fd_map;
    std::unordered_map<int, timer_entry> timer_map;
    uint64_t next_timer_id = 1;
    std::vector<std::coroutine_handle<>> ready_queue;
    bool running = false;

    impl() : epoll_fd(epoll_create1(EPOLL_CLOEXEC)) {}

    ~impl() {
        for (auto& [tfd, entry] : timer_map) {
            ::close(tfd);
        }
        ::close(epoll_fd);
    }

    void watch_fd(int fd, uint32_t events, std::coroutine_handle<> h, bool is_read) {
        auto it = fd_map.find(fd);
        if (it == fd_map.end()) {
            auto& state = fd_map[fd];
            if (is_read) {
                state.read_handle = h;
            } else {
                state.write_handle = h;
            }
            epoll_event ev{};
            ev.events = events | EPOLLONESHOT;
            ev.data.fd = fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
            }
        } else {
            if (is_read) {
                it->second.read_handle = h;
            } else {
                it->second.write_handle = h;
            }
            uint32_t combined = EPOLLONESHOT;
            if (it->second.read_handle) {
                combined |= EPOLLIN;
            }
            if (it->second.write_handle) {
                combined |= EPOLLOUT;
            }
            epoll_event ev{};
            ev.events = combined;
            ev.data.fd = fd;
            epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    void poll(int timeout_ms) {
        std::array<epoll_event, 64> events{};
        int n = epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), timeout_ms);

        for (int i = 0; i < n; ++i) {
            int fd = events[static_cast<size_t>(i)].data.fd;
            uint32_t ev = events[static_cast<size_t>(i)].events;

            auto timer_it = timer_map.find(fd);
            if (timer_it != timer_map.end()) {
                uint64_t buf{};
                [[maybe_unused]] auto bytes = ::read(fd, &buf, sizeof(buf));
                ready_queue.push_back(timer_it->second.handle);
                ::close(fd);
                timer_map.erase(timer_it);
                continue;
            }

            auto fd_it = fd_map.find(fd);
            if (fd_it == fd_map.end()) {
                continue;
            }

            auto& state = fd_it->second;
            if ((ev & EPOLLIN) && state.read_handle) {
                ready_queue.push_back(std::exchange(state.read_handle, nullptr));
            }
            if ((ev & EPOLLOUT) && state.write_handle) {
                ready_queue.push_back(std::exchange(state.write_handle, nullptr));
            }
            if (!state.read_handle && !state.write_handle) {
                fd_map.erase(fd_it);
            }
        }

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

void io_context::watch_read(int fd, std::coroutine_handle<> h) {
    impl_->watch_fd(fd, EPOLLIN, h, true);
}

void io_context::watch_write(int fd, std::coroutine_handle<> h) {
    impl_->watch_fd(fd, EPOLLOUT, h, false);
}

void io_context::cancel(int fd) {
    epoll_ctl(impl_->epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    auto it = impl_->fd_map.find(fd);
    if (it != impl_->fd_map.end()) {
        if (it->second.read_handle) {
            impl_->ready_queue.push_back(it->second.read_handle);
        }
        if (it->second.write_handle) {
            impl_->ready_queue.push_back(it->second.write_handle);
        }
        impl_->fd_map.erase(it);
    }
}

uint64_t io_context::add_timer(std::chrono::steady_clock::time_point deadline,
                               std::coroutine_handle<> h) {
    uint64_t id = impl_->next_timer_id++;
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    auto dur = deadline - std::chrono::steady_clock::now();
    if (dur.count() < 0) {
        dur = std::chrono::steady_clock::duration::zero();
    }
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
    auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(dur - secs);

    itimerspec ts{};
    ts.it_value.tv_sec = secs.count();
    ts.it_value.tv_nsec = nsecs.count();
    if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0) {
        ts.it_value.tv_nsec = 1;
    }
    timerfd_settime(tfd, 0, &ts, nullptr);

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = tfd;
    epoll_ctl(impl_->epoll_fd, EPOLL_CTL_ADD, tfd, &ev);

    impl_->timer_map[tfd] = timer_entry{id, tfd, h};
    return id;
}

void io_context::cancel_timer(uint64_t id) {
    for (auto it = impl_->timer_map.begin(); it != impl_->timer_map.end(); ++it) {
        if (it->second.id == id) {
            ::close(it->second.timerfd);
            impl_->timer_map.erase(it);
            return;
        }
    }
}

void io_context::schedule(std::coroutine_handle<> h) {
    impl_->ready_queue.push_back(h);
}

}  // namespace mirage::io

#endif
