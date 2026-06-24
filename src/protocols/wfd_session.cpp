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
    receiver_session_observer* observer = nullptr;
    bool running = false;
    impl(io::io_context& ctx, uint16_t port, receiver_session_observer* session_observer)
        : acceptor(io::tcp_acceptor::bind(ctx, port)), observer(session_observer) {}
};
wfd_session::wfd_session(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
wfd_session::~wfd_session() = default;
wfd_session::wfd_session(wfd_session&&) noexcept = default;
wfd_session& wfd_session::operator=(wfd_session&&) noexcept = default;
result<wfd_session> wfd_session::bind(io::io_context& ctx, uint16_t port,
                                      receiver_session_observer* observer) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx, port, observer);
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

receiver_client_stream_status control_stream_status(std::string reason) {
    return {
        .kind = "control",
        .health = "clean",
        .reason = std::move(reason),
    };
}

receiver_client_stream_status media_control_status(std::string reason,
                                                   std::string health = "clean") {
    return {
        .kind = "media",
        .health = std::move(health),
        .reason = std::move(reason),
    };
}

receiver_client_media_status wfd_media_status(bool active, std::string title) {
    return {
        .active = active,
        .title = std::move(title),
        .artist = "wfd",
        .album = {},
        .artwork_type = {},
        .artwork_bytes = 0,
        .position_ms = 0,
        .duration_ms = 0,
        .volume_db = 0.0F,
        .volume_linear = 1.0F,
    };
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

io::task<void> handle_wfd_connection(io::tcp_stream socket, receiver_session_observer* observer,
                                     uint64_t client_status_id) {
    wfd::control_session_state state;
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
            auto response = wfd::handle_control_request(*parsed, *body, state);
            if (!response) {
                mirage::log::debug("wfd: rejected request: {}", response.error().message);
                co_return;
            }
            auto wire = wfd::serialize_control_response(*response, parsed->cseq);
            co_await socket.async_write(byte_view(wire));
            if (response->event == wfd::control_event::client_rtp_ports_configured) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(
                        client_status_id,
                        control_stream_status(std::format(
                            "rtp_ports_configured:{}",
                            response->event_detail.empty() ? "unknown"
                                                           : response->event_detail)));
                }
                mirage::log::diagnostic(
                    "Miracast control: client_rtp_port={}",
                    response->event_detail.empty() ? "unknown" : response->event_detail);
            } else if (response->event == wfd::control_event::media_trigger_requested) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(
                        client_status_id,
                        control_stream_status(std::format(
                            "trigger_accepted:{}",
                            response->event_detail.empty() ? "unknown"
                                                           : response->event_detail)));
                }
                mirage::log::diagnostic(
                    "Miracast control: trigger={} accepted",
                    response->event_detail);
            } else if (response->event == wfd::control_event::media_setup_accepted) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(
                        client_status_id,
                        media_control_status(std::format(
                            "setup_accepted_no_renderer:{}",
                            response->event_detail.empty() ? "unknown"
                                                           : response->event_detail)));
                    observer->client_media_updated(
                        client_status_id, wfd_media_status(true, "miracast session"));
                }
                mirage::log::diagnostic(
                    "Miracast control: media_setup accepted, client_rtp_port={}, "
                    "renderer=unsupported",
                    response->event_detail.empty() ? "unknown" : response->event_detail);
            } else if (response->event == wfd::control_event::media_play_requested) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(
                        client_status_id, media_control_status("playing_no_renderer"));
                    observer->client_media_updated(
                        client_status_id, wfd_media_status(true, "miracast playing"));
                }
                mirage::log::diagnostic(
                    "Miracast control: playback=play, renderer=unsupported");
            } else if (response->event == wfd::control_event::media_pause_requested) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(
                        client_status_id, media_control_status("paused_no_renderer"));
                    observer->client_media_updated(
                        client_status_id, wfd_media_status(true, "miracast paused"));
                }
                mirage::log::diagnostic(
                    "Miracast control: playback=pause, renderer=unsupported");
            } else if (response->event == wfd::control_event::teardown_requested) {
                if (observer != nullptr && client_status_id != 0) {
                    observer->client_stream_updated(client_status_id,
                                                    media_control_status("torn_down"));
                    observer->client_media_updated(client_status_id, wfd_media_status(false, ""));
                }
                mirage::log::diagnostic(
                    "Miracast control: teardown={}, session closed",
                    response->event_detail);
            } else if (response->event == wfd::control_event::unsupported_parameter) {
                mirage::log::diagnostic(
                    "Miracast control: unsupported_parameter={}",
                    response->event_detail.empty() ? "unknown" : response->event_detail);
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

io::task<void> handle_observed_wfd_connection(io::tcp_stream socket,
                                              receiver_session_observer* observer,
                                              uint64_t client_status_id) {
    co_await handle_wfd_connection(std::move(socket), observer, client_status_id);
    if (observer != nullptr && client_status_id != 0) {
        observer->client_disconnected(client_status_id);
    }
}

}  // namespace

io::task<void> wfd_session::run() {
    impl_->running = true;
    while (impl_->running) {
        try {
            auto socket = co_await impl_->acceptor.async_accept();
            uint64_t client_status_id = 0;
            if (impl_->observer != nullptr) {
                client_status_id = impl_->observer->client_connected({
                    .id = 0,
                    .protocol_id = protocol::miracast,
                    .name = "",
                    .address = socket.remote_endpoint().addr.to_string(),
                    .state = receiver_client_state::connected,
                    .connected_at = 0,
                    .media = {},
                    .streams = {},
                });
                impl_->observer->client_stream_updated(client_status_id,
                                                       control_stream_status("connected"));
            }
            mirage::log::info("wfd connection from {}", socket.remote_endpoint().addr.to_string());
            io::co_spawn(impl_->acceptor.context(),
                         handle_observed_wfd_connection(std::move(socket), impl_->observer,
                                                        client_status_id));
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
