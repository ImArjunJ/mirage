#include <memory>
#include <string>

#include "protocols/receiver_sessions.hpp"
#include "protocols/receiver_sources.hpp"

namespace mirage::protocols {
namespace {

result<std::unique_ptr<receiver_session>> create_cast_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    return make_cast_receiver_session(*runtime.io_context, source,
                                      std::string(runtime.device_name));
}

}  // namespace

receiver_source_descriptor make_cast_receiver_source(const config& cfg) {
    return {
        .id = protocol::cast,
        .port = cfg.cast_port,
        .enabled = cfg.enable_cast,
        .experimental = true,
        .detail = "cast v2 probe receiver",
        .capabilities =
            {
                .network_listener = true,
                .discovery = true,
                .pairing = false,
                .media_setup = false,
                .audio = false,
                .video = false,
                .remote_control = false,
                .metadata = false,
                .transport = "cast-v2",
            },
        .session_factory = create_cast_session,
    };
}

}  // namespace mirage::protocols
