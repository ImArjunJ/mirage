#include <cstdint>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/core.hpp"
#include "core/log.hpp"
#include "io/io.hpp"
#include "protocols/cast/probe.hpp"
#include "protocols/protocols.hpp"
namespace mirage::protocols {
struct cast_receiver::impl {
    io::tcp_acceptor acceptor;
    std::string device_name;
    bool running = false;
    impl(io::io_context& ctx, uint16_t port, std::string name)
        : acceptor(io::tcp_acceptor::bind(ctx, port)), device_name(std::move(name)) {}
};
cast_receiver::cast_receiver(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
cast_receiver::~cast_receiver() = default;
cast_receiver::cast_receiver(cast_receiver&&) noexcept = default;
cast_receiver& cast_receiver::operator=(cast_receiver&&) noexcept = default;
result<cast_receiver> cast_receiver::bind(io::io_context& ctx, uint16_t port,
                                          std::string device_name) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx, port, std::move(device_name));
        mirage::log::info("cast probe receiver bound to port {}", port);
        return cast_receiver{std::move(impl_ptr)};
    } catch (const std::exception& e) {
        return std::unexpected(
            mirage_error::network(std::format("failed to bind cast receiver: {}", e.what())));
    }
}

namespace {

io::task<void> handle_cast_connection(io::tcp_stream socket, std::string device_name) {
    std::array<std::byte, 2048> buffer{};
    try {
        auto n = co_await socket.async_read(buffer);
        auto data =
            std::string_view(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(n));
        auto response = cast::handle_probe(data, device_name);
        switch (response.kind) {
            case cast::probe_kind::http_options:
            case cast::probe_kind::http_status:
                mirage::log::debug("cast probe: answered HTTP request");
                break;
            case cast::probe_kind::tls_client_hello:
                mirage::log::debug(
                    "cast probe: cast v2 TLS client connected, media channel is not implemented");
                break;
            case cast::probe_kind::unsupported:
                mirage::log::debug("cast probe: unsupported first packet");
                break;
        }
        if (!response.response.empty()) {
            co_await socket.async_write(
                std::as_bytes(std::span<const char>(response.response.data(),
                                                    response.response.size())));
        }
    } catch (const std::system_error& e) {
        mirage::log::debug("cast probe connection closed: {}", e.what());
    }
    socket.close();
}

}  // namespace

io::task<void> cast_receiver::run() {
    impl_->running = true;
    while (impl_->running) {
        try {
            auto socket = co_await impl_->acceptor.async_accept();
            mirage::log::info("cast probe connection from {}",
                              socket.remote_endpoint().addr.to_string());
            io::co_spawn(impl_->acceptor.context(),
                         handle_cast_connection(std::move(socket), impl_->device_name));
        } catch (const std::system_error& e) {
            if (impl_->running && e.code() != std::errc::operation_canceled) {
                mirage::log::warn("cast accept error: {}", e.what());
            }
        }
    }
}
void cast_receiver::stop() {
    impl_->running = false;
    impl_->acceptor.close();
}
}  // namespace mirage::protocols
