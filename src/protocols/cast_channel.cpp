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
#include "protocols/cast/framing.hpp"
#include "protocols/cast/message.hpp"
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

std::span<const std::byte> byte_view(std::string_view data) {
    return std::as_bytes(std::span<const char>(data.data(), data.size()));
}

io::task<void> handle_ready_frames(io::tcp_stream& socket, cast::channel_frame_parser& parser,
                                   std::string_view device_name) {
    while (auto frame = parser.next_frame()) {
        auto message = cast::parse_channel_message(*frame);
        if (!message) {
            mirage::log::debug("cast channel: failed to parse message: {}",
                               message.error().message);
            continue;
        }
        mirage::log::debug("cast channel: {} -> {} namespace={} payload={} bytes",
                           message->source_id, message->destination_id, message->namespace_,
                           message->payload_utf8.size() + message->payload_binary.size());
        auto responses = cast::handle_channel_message(*message, device_name);
        for (const auto& response : responses) {
            auto payload = cast::serialize_channel_message(response);
            if (!payload) {
                mirage::log::debug("cast channel: failed to serialize response: {}",
                                   payload.error().message);
                continue;
            }
            auto framed = cast::make_channel_frame(*payload);
            if (!framed) {
                mirage::log::debug("cast channel: failed to frame response: {}",
                                   framed.error().message);
                continue;
            }
            co_await socket.async_write(std::span<const std::byte>(framed->data(),
                                                                    framed->size()));
        }
    }
}

io::task<void> handle_cast_channel(io::tcp_stream& socket, std::string_view first_packet,
                                   std::string_view device_name) {
    cast::channel_frame_parser parser;
    auto appended = parser.append(byte_view(first_packet));
    if (!appended) {
        mirage::log::debug("cast channel: rejected frame prefix: {}", appended.error().message);
        co_return;
    }
    co_await handle_ready_frames(socket, parser, device_name);

    std::array<std::byte, 2048> buffer{};
    while (socket.is_open()) {
        auto n = co_await socket.async_read(buffer);
        if (n == 0) {
            co_return;
        }
        appended = parser.append(std::span<const std::byte>(buffer.data(), n));
        if (!appended) {
            mirage::log::debug("cast channel: rejected frame: {}", appended.error().message);
            co_return;
        }
        co_await handle_ready_frames(socket, parser, device_name);
    }
}

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
            case cast::probe_kind::channel_frame:
                mirage::log::debug(
                    "cast channel: plaintext frame stream connected, control/status enabled");
                co_await handle_cast_channel(socket, data, device_name);
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
