#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "core/core.hpp"
#include "io/tcp.hpp"

struct ssl_ctx_st;
struct ssl_st;

namespace mirage::protocols::cast {

class tls_channel {
public:
    tls_channel(const tls_channel&) = delete;
    tls_channel& operator=(const tls_channel&) = delete;

    tls_channel(tls_channel&&) noexcept;
    tls_channel& operator=(tls_channel&&) noexcept;

    ~tls_channel();

    static io::task<result<tls_channel>> accept(io::tcp_stream socket,
                                                std::span<const std::byte> first_packet);

    io::task<result<size_t>> async_read(std::span<std::byte> data);
    io::task<result<void>> async_write(std::span<const std::byte> data);

    [[nodiscard]] bool is_open() const;
    void close();

    struct ssl_ctx_deleter {
        void operator()(ssl_ctx_st* ctx) const;
    };

    struct ssl_deleter {
        void operator()(ssl_st* ssl) const;
    };

private:
    tls_channel(io::tcp_stream socket, std::unique_ptr<ssl_ctx_st, ssl_ctx_deleter> ctx,
                std::unique_ptr<ssl_st, ssl_deleter> ssl);

    result<void> feed_encrypted(std::span<const std::byte> data);
    io::task<result<void>> flush_pending();
    io::task<result<void>> read_encrypted_packet();
    io::task<result<void>> handshake();

    io::tcp_stream socket_;
    std::unique_ptr<ssl_ctx_st, ssl_ctx_deleter> ctx_;
    std::unique_ptr<ssl_st, ssl_deleter> ssl_;
};

}  // namespace mirage::protocols::cast
