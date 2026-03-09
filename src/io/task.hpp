#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

namespace mirage::io {

template <typename T = void>
class task;

namespace detail {

struct final_awaiter {
    [[nodiscard]] bool await_ready() noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) noexcept;

    void await_resume() noexcept {}
};

struct task_promise_base {
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr exception;

    [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
    [[nodiscard]] final_awaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { exception = std::current_exception(); }
};

inline std::coroutine_handle<> final_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    auto& promise = std::coroutine_handle<task_promise_base>::from_address(h.address()).promise();
    return promise.continuation;
}

template <typename T>
struct task_promise : task_promise_base {
    using handle_type = std::coroutine_handle<task_promise>;

    std::variant<std::monostate, T> result;

    task<T> get_return_object();

    void return_value(T value) { result.template emplace<1>(std::move(value)); }

    [[nodiscard]] T get() {
        if (exception) {
            std::rethrow_exception(exception);
        }
        return std::move(std::get<1>(result));
    }
};

template <>
struct task_promise<void> : task_promise_base {
    using handle_type = std::coroutine_handle<task_promise>;

    task<void> get_return_object();

    void return_void() {}

    void get() {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

}  // namespace detail

template <typename T>
class task {
public:
    using promise_type = detail::task_promise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) : handle_(h) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() {
        if (handle_) {
            handle_.destroy();
        }
    }

    [[nodiscard]] handle_type release() noexcept { return std::exchange(handle_, nullptr); }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation = caller;
        return handle_;
    }

    decltype(auto) await_resume() { return handle_.promise().get(); }

private:
    handle_type handle_;
};

namespace detail {

template <typename T>
task<T> task_promise<T>::get_return_object() {
    return task<T>{handle_type::from_promise(*this)};
}

inline task<void> task_promise<void>::get_return_object() {
    return task<void>{handle_type::from_promise(*this)};
}

}  // namespace detail

}  // namespace mirage::io
