#pragma once
#include <chrono>
#include <format>
#include <print>
#include <string_view>
#include <utility>

namespace mirage::log {

enum class level : int { trace = 0, debug = 1, info = 2, user = 3, warn = 4, error = 5 };

inline level min_level = level::user;
inline bool diagnostics_enabled = false;

namespace detail {
inline void emit(std::string_view tag, std::string_view msg) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::floor<std::chrono::milliseconds>(now);
    std::println(stderr, "[{:%Y-%m-%d %H:%M:%S}] [{}] {}", time, tag, msg);
}
}  // namespace detail

template <typename... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (min_level <= level::trace) {
        detail::emit("trace", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (min_level <= level::debug) {
        detail::emit("debug", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (min_level <= level::info) {
        detail::emit("info", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void user(std::format_string<Args...> fmt, Args&&... args) {
    if (min_level <= level::user) {
        std::println(stderr, "{}", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void diagnostic(std::format_string<Args...> fmt, Args&&... args) {
    if (diagnostics_enabled || min_level <= level::info) {
        detail::emit("diag", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (min_level <= level::warn) {
        detail::emit("warn", std::format(fmt, std::forward<Args>(args)...));
    }
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    detail::emit("error", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace mirage::log
