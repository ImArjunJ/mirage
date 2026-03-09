#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "io/address.hpp"
#include "io/platform.hpp"
#include "io/task.hpp"

namespace mirage::io {

class io_context;

class tcp_stream {
public:
    tcp_stream(io_context& ctx, socket_t fd);

    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;

    tcp_stream(tcp_stream&& other) noexcept;
    tcp_stream& operator=(tcp_stream&& other) noexcept;

    ~tcp_stream();

    void close();

    task<size_t> async_read(std::span<std::byte> buf);
    task<void> async_read_exactly(std::span<std::byte> buf);
    task<void> async_write(std::span<const std::byte> data);
    task<std::string> async_read_until(std::string_view delim);

    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] endpoint remote_endpoint() const;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] socket_t native_handle() const;
    [[nodiscard]] io_context& context() const;

    std::string& buffer();

private:
    io_context* ctx_;
    socket_t fd_;
    std::string read_buffer_;
};

class tcp_acceptor {
public:
    static tcp_acceptor bind(io_context& ctx, uint16_t port);
    static tcp_acceptor bind(io_context& ctx, endpoint ep);

    tcp_acceptor(const tcp_acceptor&) = delete;
    tcp_acceptor& operator=(const tcp_acceptor&) = delete;

    tcp_acceptor(tcp_acceptor&& other) noexcept;
    tcp_acceptor& operator=(tcp_acceptor&& other) noexcept;

    ~tcp_acceptor();

    task<tcp_stream> async_accept();

    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] io_context& context() const;

    void close();
    void listen(int backlog) const;

private:
    tcp_acceptor(io_context& ctx, socket_t fd);

    io_context* ctx_;
    socket_t fd_;
};

}  // namespace mirage::io
