#include "protocols/receiver_sessions.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mirage::protocols {
namespace {

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
    };
}

}  // namespace

std::vector<receiver_source_descriptor> make_receiver_source_descriptors(const config& cfg) {
    return {
        airplay_source(cfg),
        cast_source(cfg),
        wfd_source(cfg),
    };
}

result<std::unique_ptr<receiver_session>> make_receiver_session(
    io::io_context& ctx, const receiver_source_descriptor& source,
    crypto::ed25519_keypair* airplay_keypair, std::string device_name, std::string mac_address) {
    switch (source.id) {
        case protocol::airplay:
            if (airplay_keypair == nullptr) {
                return std::unexpected(
                    mirage_error::crypto("airplay receiver identity is not available"));
            }
            return make_airplay_receiver_session(ctx, source, std::move(*airplay_keypair),
                                                 std::move(device_name), std::move(mac_address));
        case protocol::cast:
            return make_cast_receiver_session(ctx, source, std::move(device_name));
        case protocol::miracast:
            return make_wfd_receiver_session(ctx, source);
    }
    std::unreachable();
}

}  // namespace mirage::protocols
