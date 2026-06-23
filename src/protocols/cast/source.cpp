#include <memory>
#include <string>
#include <utility>

#include "core/receiver_identity.hpp"
#include "protocols/receiver_sessions.hpp"
#include "protocols/receiver_sources.hpp"

namespace mirage::protocols {
namespace {

result<std::unique_ptr<receiver_session>> create_cast_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    if (runtime.receiver_public_key == nullptr) {
        return std::unexpected(mirage_error::crypto("cast receiver identity is not available"));
    }
    auto identity = derive_protocol_identity(*runtime.receiver_public_key, "cast-v2");
    return make_cast_receiver_session(*runtime.io_context, source, std::string(runtime.device_name),
                                      std::move(identity), runtime.session_observer);
}

}  // namespace

receiver_source_descriptor make_cast_receiver_source(const config& cfg) {
    return {
        .id = protocol::cast,
        .port = cfg.cast_port,
        .enabled = cfg.enable_cast,
        .experimental = true,
        .detail = "cast v2 app/media control receiver",
        .capabilities =
            {
                .network_listener = true,
                .discovery = true,
                .pairing = false,
                .media_setup = true,
                .audio = false,
                .video = false,
                .remote_control = true,
                .app_lifecycle = true,
                .media_control = true,
                .metadata = true,
                .transport = "cast-v2",
            },
        .session_factory = create_cast_session,
    };
}

}  // namespace mirage::protocols
