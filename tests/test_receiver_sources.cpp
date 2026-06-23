#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "protocols/receiver_sessions.hpp"
#include "protocols/receiver_sources.hpp"

namespace mirage::protocols {

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx,
                                                                receiver_source_descriptor source,
                                                                crypto::ed25519_keypair keypair,
                                                                std::string device_name,
                                                                std::string mac_address,
                                                                receiver_session_observer* observer) {
    static_cast<void>(ctx);
    static_cast<void>(source);
    static_cast<void>(keypair);
    static_cast<void>(device_name);
    static_cast<void>(mac_address);
    static_cast<void>(observer);
    return {};
}

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx,
                                                             receiver_source_descriptor source,
                                                             std::string device_name,
                                                             protocol_receiver_identity identity,
                                                             receiver_session_observer* observer) {
    static_cast<void>(ctx);
    static_cast<void>(source);
    static_cast<void>(device_name);
    static_cast<void>(identity);
    static_cast<void>(observer);
    return {};
}

std::unique_ptr<receiver_session> make_wfd_receiver_session(io::io_context& ctx,
                                                            receiver_source_descriptor source,
                                                            receiver_session_observer* observer) {
    static_cast<void>(ctx);
    static_cast<void>(source);
    static_cast<void>(observer);
    return {};
}

}  // namespace mirage::protocols

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

const mirage::receiver_source_descriptor* find_source(
    const std::vector<mirage::receiver_source_descriptor>& sources, mirage::protocol id) {
    auto it = std::ranges::find(sources, id, &mirage::receiver_source_descriptor::id);
    if (it == sources.end()) {
        return nullptr;
    }
    return &*it;
}

bool expect_audio_video_remote_metadata(const mirage::receiver_source_descriptor& source,
                                        std::string_view transport) {
    bool ok = true;
    ok &= expect(source.capabilities.network_listener, "source listener capability mismatch");
    ok &= expect(source.capabilities.discovery, "source discovery capability mismatch");
    ok &= expect(source.capabilities.media_setup, "source media setup capability mismatch");
    ok &= expect(source.capabilities.audio, "source audio capability mismatch");
    ok &= expect(source.capabilities.video, "source video capability mismatch");
    ok &= expect(source.capabilities.remote_control, "source remote control capability mismatch");
    ok &= expect(source.capabilities.metadata, "source metadata capability mismatch");
    ok &= expect(source.capabilities.transport == transport, "source transport mismatch");
    return ok;
}

bool expect_no_media_capabilities(const mirage::receiver_source_descriptor& source,
                                  std::string_view transport) {
    bool ok = true;
    ok &= expect(!source.capabilities.pairing, "source pairing capability mismatch");
    ok &= expect(!source.capabilities.media_setup, "source media setup capability mismatch");
    ok &= expect(!source.capabilities.audio, "source audio capability mismatch");
    ok &= expect(!source.capabilities.video, "source video capability mismatch");
    ok &= expect(!source.capabilities.remote_control, "source remote control capability mismatch");
    ok &= expect(!source.capabilities.metadata, "source metadata capability mismatch");
    ok &= expect(source.capabilities.transport == transport, "source transport mismatch");
    return ok;
}

}  // namespace

int main() {
    bool ok = true;

    mirage::config defaults;
    auto sources = mirage::protocols::make_receiver_source_descriptors(defaults);
    ok &= expect(sources.size() == 3, "source count mismatch");
    ok &= expect(sources.size() >= 3 && sources[0].id == mirage::protocol::airplay,
                 "airplay source order mismatch");
    ok &= expect(sources.size() >= 3 && sources[1].id == mirage::protocol::cast,
                 "cast source order mismatch");
    ok &= expect(sources.size() >= 3 && sources[2].id == mirage::protocol::miracast,
                 "miracast source order mismatch");
    if (!ok) {
        return 1;
    }

    const auto* airplay = find_source(sources, mirage::protocol::airplay);
    const auto* cast = find_source(sources, mirage::protocol::cast);
    const auto* wfd = find_source(sources, mirage::protocol::miracast);

    ok &= expect(airplay != nullptr, "airplay source missing");
    ok &= expect(cast != nullptr, "cast source missing");
    ok &= expect(wfd != nullptr, "miracast source missing");
    if (!ok) {
        return 1;
    }

    ok &= expect(airplay->enabled, "airplay default enabled mismatch");
    ok &= expect(!cast->enabled, "cast default enabled mismatch");
    ok &= expect(!wfd->enabled, "miracast default enabled mismatch");

    ok &= expect(airplay->port == 7000, "airplay default port mismatch");
    ok &= expect(cast->port == 8009, "cast default port mismatch");
    ok &= expect(wfd->port == 7236, "miracast default port mismatch");

    ok &= expect(airplay->experimental, "airplay experimental flag mismatch");
    ok &= expect(cast->experimental, "cast experimental flag mismatch");
    ok &= expect(wfd->experimental, "miracast experimental flag mismatch");

    ok &= expect(airplay->detail == std::string_view("rtsp/raop receiver"),
                 "airplay detail mismatch");
    ok &= expect(cast->detail == std::string_view("cast v2 control/status receiver"),
                 "cast detail mismatch");
    ok &= expect(wfd->detail == std::string_view("wfd capability listener"),
                 "miracast detail mismatch");

    ok &= expect(airplay->validate_source != nullptr, "airplay validator missing");
    ok &= expect(airplay->session_factory != nullptr, "airplay factory missing");
    ok &= expect(cast->session_factory != nullptr, "cast factory missing");
    ok &= expect(wfd->session_factory != nullptr, "miracast factory missing");

    ok &= expect(airplay->capabilities.pairing, "airplay pairing capability mismatch");
    ok &= expect_audio_video_remote_metadata(*airplay, "rtsp/raop");
    ok &= expect(cast->capabilities.network_listener, "cast listener capability mismatch");
    ok &= expect(cast->capabilities.discovery, "cast discovery capability mismatch");
    ok &= expect(!cast->capabilities.pairing, "cast pairing capability mismatch");
    ok &= expect(!cast->capabilities.media_setup, "cast media setup capability mismatch");
    ok &= expect(!cast->capabilities.audio, "cast audio capability mismatch");
    ok &= expect(!cast->capabilities.video, "cast video capability mismatch");
    ok &= expect(cast->capabilities.remote_control, "cast remote control capability mismatch");
    ok &= expect(!cast->capabilities.metadata, "cast metadata capability mismatch");
    ok &= expect(cast->capabilities.transport == "cast-v2", "cast transport mismatch");

    ok &= expect(wfd->capabilities.network_listener, "miracast listener capability mismatch");
    ok &= expect(!wfd->capabilities.discovery, "miracast discovery capability mismatch");
    ok &= expect_no_media_capabilities(*wfd, "wfd");

    ok &= expect(mirage::classify_audio_stream({}) == mirage::receiver_stream_health::clean,
                 "idle audio health mismatch");
    ok &= expect(mirage::audio_stream_health_reason({}) == "no_audio_packets",
                 "idle audio reason mismatch");
    ok &= expect(mirage::classify_audio_stream({
                     .received_packets = 8,
                     .decoded_packets = 0,
                     .silent_or_marker = 0,
                 }) == mirage::receiver_stream_health::attention,
                 "undecoded audio health mismatch");
    ok &= expect(mirage::audio_stream_health_reason({
                     .received_packets = 8,
                     .decoded_packets = 0,
                     .silent_or_marker = 0,
                 }) == "no_decoded_audio",
                 "undecoded audio reason mismatch");
    ok &= expect(mirage::classify_audio_stream({
                     .received_packets = 16,
                     .decoded_packets = 8,
                     .silent_or_marker = 0,
                     .stale_or_redundant = 100,
                 }) == mirage::receiver_stream_health::clean,
                 "redundant audio health mismatch");
    ok &= expect(mirage::classify_video_stream({
                     .frames = 120,
                     .keyframes = 1,
                     .decrypted_frames = 119,
                 }) == mirage::receiver_stream_health::clean,
                 "clean video health mismatch");
    ok &= expect(mirage::video_stream_health_reason({
                     .frames = 120,
                     .keyframes = 0,
                 }) == "no_keyframes",
                 "video keyframe reason mismatch");
    ok &= expect(mirage::classify_video_stream({
                     .frames = 120,
                     .keyframes = 1,
                     .decrypt_failures = 1,
                 }) == mirage::receiver_stream_health::attention,
                 "video decrypt failure health mismatch");

    mirage::config custom;
    custom.enable_cast = true;
    custom.enable_miracast = true;
    custom.airplay_port = 7100;
    custom.cast_port = 8100;
    custom.miracast_port = 7300;

    auto custom_sources = mirage::protocols::make_receiver_source_descriptors(custom);
    const auto* custom_airplay = find_source(custom_sources, mirage::protocol::airplay);
    const auto* custom_cast = find_source(custom_sources, mirage::protocol::cast);
    const auto* custom_wfd = find_source(custom_sources, mirage::protocol::miracast);
    ok &=
        expect(custom_airplay != nullptr && custom_airplay->enabled && custom_airplay->port == 7100,
               "custom airplay source mismatch");
    ok &= expect(custom_cast != nullptr && custom_cast->enabled && custom_cast->port == 8100,
                 "custom cast source mismatch");
    ok &= expect(custom_wfd != nullptr && custom_wfd->enabled && custom_wfd->port == 7300,
                 "custom miracast source mismatch");

    return ok ? 0 : 1;
}
