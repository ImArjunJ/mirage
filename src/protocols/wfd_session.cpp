#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "core/core.hpp"
#include "core/log.hpp"
#include "io/io.hpp"
#include "protocols/protocols.hpp"
#include "protocols/rtsp_message.hpp"
#include "protocols/wfd/control.hpp"
namespace mirage::protocols {
struct wfd_session::impl {
    io::tcp_acceptor acceptor;
    bool running = false;
    impl(io::io_context& ctx, uint16_t port) : acceptor(io::tcp_acceptor::bind(ctx, port)) {}
};
wfd_session::wfd_session(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
wfd_session::~wfd_session() = default;
wfd_session::wfd_session(wfd_session&&) noexcept = default;
wfd_session& wfd_session::operator=(wfd_session&&) noexcept = default;
result<wfd_session> wfd_session::bind(io::io_context& ctx, uint16_t port) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx, port);
        mirage::log::info("WFD (Miracast) capability listener bound to port {}", port);
        return wfd_session{std::move(impl_ptr)};
    } catch (const std::exception& e) {
        return std::unexpected(
            mirage_error::network(std::format("failed to bind WFD listener: {}", e.what())));
    }
}

namespace {

std::span<const std::byte> byte_view(std::string_view data) {
    return std::as_bytes(std::span<const char>(data.data(), data.size()));
}

io::task<result<std::string>> read_body(io::tcp_stream& socket, size_t content_length) {
    std::string body;
    auto& leftover = socket.buffer();
    auto from_buffer = std::min(leftover.size(), content_length);
    if (from_buffer > 0) {
        body.append(leftover.data(), from_buffer);
        leftover.erase(0, from_buffer);
    }
    if (body.size() < content_length) {
        auto to_read = content_length - body.size();
        std::string remaining(to_read, '\0');
        co_await socket.async_read_exactly(
            std::as_writable_bytes(std::span<char>(remaining.data(), remaining.size())));
        body += remaining;
    }
    co_return body;
}

io::task<void> handle_wfd_connection(io::tcp_stream socket) {
    try {
        while (socket.is_open()) {
            auto header = co_await socket.async_read_until("\r\n\r\n");
            auto parsed = parse_rtsp_request_head(header);
            if (!parsed) {
                mirage::log::debug("wfd: invalid RTSP request: {}", parsed.error().message);
                co_return;
            }

            auto body = co_await read_body(socket, parsed->content_length);
            if (!body) {
                mirage::log::debug("wfd: failed to read body: {}", body.error().message);
                co_return;
            }

            mirage::log::debug("wfd: {} {} {}", parsed->method, parsed->uri, parsed->version);
            auto response = wfd::handle_control_request(*parsed, *body);
            if (!response) {
                mirage::log::debug("wfd: rejected request: {}", response.error().message);
                co_return;
            }
            auto wire = wfd::serialize_control_response(*response, parsed->cseq);
            co_await socket.async_write(byte_view(wire));
            if (response->status_code == 501) {
                mirage::log::diagnostic(
                    "Miracast stream summary: health=attention, reason=media_not_implemented");
            }
            if (response->close_after_send) {
                co_return;
            }
        }
    } catch (const std::system_error& e) {
        mirage::log::debug("wfd connection closed: {}", e.what());
    } catch (const std::exception& e) {
        mirage::log::debug("wfd connection failed: {}", e.what());
    }
}

}  // namespace

io::task<void> wfd_session::run() {
    impl_->running = true;
    while (impl_->running) {
        try {
            auto socket = co_await impl_->acceptor.async_accept();
            mirage::log::info("wfd connection from {}", socket.remote_endpoint().addr.to_string());
            io::co_spawn(impl_->acceptor.context(), handle_wfd_connection(std::move(socket)));
        } catch (const std::system_error& e) {
            if (impl_->running && e.code() != std::errc::operation_canceled) {
                mirage::log::warn("wfd accept error: {}", e.what());
            }
        }
    }
}
void wfd_session::stop() {
    impl_->running = false;
    impl_->acceptor.close();
}
}  // namespace mirage::protocols
