#include <memory>
#include <string>
#include <utility>

#include "protocols/receiver_sessions.hpp"
#include "protocols/receiver_sources.hpp"

namespace mirage::protocols {
namespace {

result<void> validate_airplay_source(const receiver_source_descriptor& source,
                                     const receiver_source_runtime& runtime) {
    static_cast<void>(source);
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    if (runtime.receiver_identity == nullptr) {
        return std::unexpected(mirage_error::crypto("airplay receiver identity is not available"));
    }
    return {};
}

result<std::unique_ptr<receiver_session>> create_airplay_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (auto valid = validate_airplay_source(source, runtime); !valid) {
        return std::unexpected(valid.error());
    }

    auto keypair = runtime.receiver_identity->clone();
    if (!keypair) {
        return std::unexpected(keypair.error());
    }

    return make_airplay_receiver_session(*runtime.io_context, source, std::move(*keypair),
                                         std::string(runtime.device_name),
                                         std::string(runtime.mac_address));
}

}  // namespace

receiver_source_descriptor make_airplay_receiver_source(const config& cfg) {
    return {
        .id = protocol::airplay,
        .port = cfg.airplay_port,
        .enabled = cfg.enable_airplay,
        .experimental = true,
        .detail = "rtsp/raop receiver",
        .capabilities =
            {
                .network_listener = true,
                .discovery = true,
                .pairing = true,
                .media_setup = true,
                .audio = true,
                .video = true,
                .remote_control = true,
                .metadata = true,
                .transport = "rtsp/raop",
            },
        .validate_source = validate_airplay_source,
        .session_factory = create_airplay_session,
    };
}

}  // namespace mirage::protocols
