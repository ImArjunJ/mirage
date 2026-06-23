#include <memory>

#include "protocols/receiver_sessions.hpp"
#include "protocols/receiver_sources.hpp"

namespace mirage::protocols {
namespace {

result<std::unique_ptr<receiver_session>> create_wfd_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    return make_wfd_receiver_session(*runtime.io_context, source, runtime.session_observer);
}

}  // namespace

receiver_source_descriptor make_wfd_receiver_source(const config& cfg) {
    return {
        .id = protocol::miracast,
        .port = cfg.miracast_port,
        .enabled = cfg.enable_miracast,
        .experimental = true,
        .detail = "wfd control/media lifecycle receiver",
        .capabilities =
            {
                .network_listener = true,
                .discovery = false,
                .pairing = false,
                .media_setup = true,
                .audio = false,
                .video = false,
                .remote_control = false,
                .app_lifecycle = false,
                .media_control = true,
                .metadata = false,
                .transport = "wfd",
            },
        .session_factory = create_wfd_session,
    };
}

}  // namespace mirage::protocols
