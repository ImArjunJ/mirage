#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "io/address.hpp"
#include "io/event_loop.hpp"
#include "io/platform.hpp"
#include "io/task.hpp"

namespace mirage::io {

class udp_socket {
public:
    static udp_socket bind(io_context& ctx, uint16_t port = 0);

    udp_socket(const udp_socket&) = delete;
    udp_socket& operator=(const udp_socket&) = delete;

    udp_socket(udp_socket&& other) noexcept;
    udp_socket& operator=(udp_socket&& other) noexcept;

    ~udp_socket();

    void close();

    task<size_t> async_recv_from(std::span<std::byte> buf, endpoint& sender);
    task<size_t> async_send_to(std::span<const std::byte> data, const endpoint& dest);

    void set_multicast_interface(const ip_address& addr);
    void join_multicast(const ip_address& group);
    void set_multicast_loopback(bool enable);
    void set_reuse_address(bool enable);

    [[nodiscard]] endpoint local_endpoint() const;
    [[nodiscard]] bool is_open() const;
    [[nodiscard]] socket_t native_handle() const;
    [[nodiscard]] io_context& context() const;

private:
    udp_socket(io_context& ctx, socket_t fd);

    io_context* ctx_;
    socket_t fd_;
};

}  // namespace mirage::io
