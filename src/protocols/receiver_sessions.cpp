#include "protocols/receiver_sessions.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirage::protocols {
namespace {

result<void> validate_airplay_source(const receiver_source_descriptor& source,
                                     const receiver_source_runtime& runtime);
result<std::unique_ptr<receiver_session>> create_airplay_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime);
result<std::unique_ptr<receiver_session>> create_cast_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime);
result<std::unique_ptr<receiver_session>> create_wfd_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime);

receiver_source_descriptor airplay_source(const config& cfg) {
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

receiver_source_descriptor cast_source(const config& cfg) {
    return {
        .id = protocol::cast,
        .port = cfg.cast_port,
        .enabled = cfg.enable_cast,
        .experimental = true,
        .detail = "cast v2 receiver",
        .capabilities =
            {
                .network_listener = true,
                .discovery = true,
                .pairing = false,
                .media_setup = true,
                .audio = true,
                .video = true,
                .remote_control = true,
                .metadata = true,
                .transport = "cast-v2",
            },
        .session_factory = create_cast_session,
    };
}

receiver_source_descriptor wfd_source(const config& cfg) {
    return {
        .id = protocol::miracast,
        .port = cfg.miracast_port,
        .enabled = cfg.enable_miracast,
        .experimental = true,
        .detail = "wfd receiver",
        .capabilities =
            {
                .network_listener = false,
                .discovery = false,
                .pairing = true,
                .media_setup = true,
                .audio = true,
                .video = true,
                .remote_control = true,
                .metadata = false,
                .transport = "wfd",
            },
        .session_factory = create_wfd_session,
    };
}

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
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    if (runtime.receiver_identity == nullptr) {
        return std::unexpected(mirage_error::crypto("airplay receiver identity is not available"));
    }

    auto keypair = runtime.receiver_identity->clone();
    if (!keypair) {
        return std::unexpected(keypair.error());
    }

    return make_airplay_receiver_session(*runtime.io_context, source, std::move(*keypair),
                                         std::string(runtime.device_name),
                                         std::string(runtime.mac_address));
}

result<std::unique_ptr<receiver_session>> create_cast_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    return make_cast_receiver_session(*runtime.io_context, source,
                                      std::string(runtime.device_name));
}

result<std::unique_ptr<receiver_session>> create_wfd_session(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime) {
    if (runtime.io_context == nullptr) {
        return std::unexpected(mirage_error::session("receiver runtime is missing an event loop"));
    }
    return make_wfd_receiver_session(*runtime.io_context, source);
}

}  // namespace

std::vector<receiver_source_descriptor> make_receiver_source_descriptors(const config& cfg) {
    return {
        airplay_source(cfg),
        cast_source(cfg),
        wfd_source(cfg),
    };
}

}  // namespace mirage::protocols
