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
#include "protocols/cast/tls.hpp"
#include "protocols/protocols.hpp"
namespace mirage::protocols {
struct cast_receiver::impl {
    io::tcp_acceptor acceptor;
    std::string device_name;
    receiver_session_observer* observer = nullptr;
    bool running = false;
    impl(io::io_context& ctx, uint16_t port, std::string name,
         receiver_session_observer* session_observer)
        : acceptor(io::tcp_acceptor::bind(ctx, port)),
          device_name(std::move(name)),
          observer(session_observer) {}
};
cast_receiver::cast_receiver(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
cast_receiver::~cast_receiver() = default;
cast_receiver::cast_receiver(cast_receiver&&) noexcept = default;
cast_receiver& cast_receiver::operator=(cast_receiver&&) noexcept = default;
result<cast_receiver> cast_receiver::bind(io::io_context& ctx, uint16_t port,
                                          std::string device_name,
                                          receiver_session_observer* observer) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx, port, std::move(device_name), observer);
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

receiver_client_stream_status control_stream_status(std::string reason) {
    return {
        .kind = "control",
        .health = "clean",
        .reason = std::move(reason),
    };
}

io::task<bool> write_cast_frame(io::tcp_stream& socket, std::span<const std::byte> frame) {
    co_await socket.async_write(frame);
    co_return true;
}

io::task<bool> write_cast_frame(cast::tls_channel& socket, std::span<const std::byte> frame) {
    auto written = co_await socket.async_write(frame);
    if (!written) {
        mirage::log::debug("cast tls channel: failed to write response: {}",
                           written.error().message);
        co_return false;
    }
    co_return true;
}

template <typename Stream>
io::task<void> handle_ready_frames(Stream& socket, cast::channel_frame_parser& parser,
                                   std::string_view device_name,
                                   cast::channel_session_state& state) {
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
        auto responses = cast::handle_channel_message(*message, device_name, state);
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
            auto written = co_await write_cast_frame(
                socket, std::span<const std::byte>(framed->data(), framed->size()));
            if (!written) {
                co_return;
            }
        }
    }
}

io::task<void> handle_cast_channel(io::tcp_stream& socket, std::string_view first_packet,
                                   std::string_view device_name,
                                   cast::channel_session_state& state) {
    cast::channel_frame_parser parser;
    auto appended = parser.append(byte_view(first_packet));
    if (!appended) {
        mirage::log::debug("cast channel: rejected frame prefix: {}", appended.error().message);
        co_return;
    }
    co_await handle_ready_frames(socket, parser, device_name, state);

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
        co_await handle_ready_frames(socket, parser, device_name, state);
    }
}

io::task<void> handle_cast_tls_channel(io::tcp_stream socket, std::string_view first_packet,
                                       std::string device_name) {
    auto accepted = co_await cast::tls_channel::accept(std::move(socket), byte_view(first_packet));
    if (!accepted) {
        mirage::log::debug("cast tls channel: handshake failed: {}", accepted.error().message);
        co_return;
    }

    auto tls = std::move(*accepted);
    mirage::log::debug("cast tls channel: control/status enabled");

    cast::channel_frame_parser parser;
    cast::channel_session_state state;
    std::array<std::byte, 2048> buffer{};
    while (tls.is_open()) {
        auto n = co_await tls.async_read(buffer);
        if (!n) {
            mirage::log::debug("cast tls channel: read failed: {}", n.error().message);
            co_return;
        }
        if (*n == 0) {
            co_return;
        }
        auto appended = parser.append(std::span<const std::byte>(buffer.data(), *n));
        if (!appended) {
            mirage::log::debug("cast tls channel: rejected frame: {}", appended.error().message);
            co_return;
        }
        co_await handle_ready_frames(tls, parser, device_name, state);
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
                    "cast tls channel: client connected, media channel is not implemented");
                co_await handle_cast_tls_channel(std::move(socket), data, device_name);
                co_return;
            case cast::probe_kind::channel_frame:
                mirage::log::debug(
                    "cast channel: plaintext frame stream connected, control/status enabled");
                {
                    cast::channel_session_state state;
                    co_await handle_cast_channel(socket, data, device_name, state);
                }
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

io::task<void> handle_observed_cast_connection(io::tcp_stream socket, std::string device_name,
                                               receiver_session_observer* observer,
                                               uint64_t client_status_id) {
    co_await handle_cast_connection(std::move(socket), std::move(device_name));
    if (observer != nullptr && client_status_id != 0) {
        observer->client_disconnected(client_status_id);
    }
}

}  // namespace

io::task<void> cast_receiver::run() {
    impl_->running = true;
    while (impl_->running) {
        try {
            auto socket = co_await impl_->acceptor.async_accept();
            uint64_t client_status_id = 0;
            if (impl_->observer != nullptr) {
                client_status_id = impl_->observer->client_connected({
                    .id = 0,
                    .protocol_id = protocol::cast,
                    .name = "",
                    .address = socket.remote_endpoint().addr.to_string(),
                    .state = "connected",
                    .connected_at = 0,
                    .streams = {},
                });
                impl_->observer->client_stream_updated(client_status_id,
                                                       control_stream_status("connected"));
            }
            mirage::log::info("cast probe connection from {}",
                              socket.remote_endpoint().addr.to_string());
            io::co_spawn(impl_->acceptor.context(),
                         handle_observed_cast_connection(std::move(socket), impl_->device_name,
                                                         impl_->observer, client_status_id));
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
