#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "core/runtime_process.hpp"
#include "core/runtime_status.hpp"
#include "core/status_report.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(const std::string& value, std::string_view needle) {
    return value.find(needle) != std::string::npos;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    bool ok = true;

    const auto root =
        std::filesystem::temp_directory_path() /
        ("mirage-runtime-status-test-" + std::to_string(mirage::current_process_id()));
    const auto status_path = root / "state" / "status.json";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    mirage::config cfg;
    cfg.device_name = "Living Room";
    cfg.airplay_port = 7000;
    cfg.cast_port = 8009;
    cfg.miracast_port = 7236;

    std::array sources{
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::airplay,
            .port = 7000,
            .enabled = true,
            .experimental = false,
            .detail = "rtsp/raop receiver",
            .capabilities =
                {
                    .audio = true,
                    .video = true,
                    .metadata = true,
                    .transport = "rtsp/raop",
                },
        },
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::cast,
            .port = 8009,
            .enabled = true,
            .experimental = true,
            .detail = "cast receiver",
            .capabilities =
                {
                    .app_lifecycle = true,
                    .media_control = true,
                    .transport = "cast-v2",
                },
        },
    };

    mirage::receiver_adapter_registry adapters(
        std::span<const mirage::receiver_source_descriptor>(sources.data(), sources.size()));
    adapters.mark_listening(mirage::protocol::airplay);
    adapters.mark_listening(mirage::protocol::cast);

    mirage::runtime_status_tracker tracker(
        status_path, 42, cfg, "192.0.2.10", "wlan0", "/tmp/mirage/identity.key", 123456, adapters,
        std::span<const mirage::receiver_source_descriptor>(sources.data(), sources.size()));
    ok &= expect(tracker.write(), "initial status write failed");

    auto json = read_text(status_path);
    ok &= expect(contains(json, "\"pid\":42"), "initial status pid missing");
    ok &= expect(contains(json, "\"clients\":[]"), "initial clients should be empty");

    mirage::receiver_client_status client;
    client.protocol_id = mirage::protocol::cast;
    client.address = "192.0.2.20";
    client.state = "connected";
    client.connected_at = 123500;
    const auto client_id = tracker.client_connected(std::move(client));
    ok &= expect(client_id == 1, "client id mismatch");
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::running,
        "adapter did not enter running state");

    tracker.client_stream_updated(client_id, {
                                                 .kind = "audio",
                                                 .health = "clean",
                                                 .reason = "ok",
                                                 .received_packets = 10,
                                                 .decoded_packets = 9,
                                             });
    tracker.client_stream_updated(client_id, {
                                                 .kind = "media",
                                                 .health = "clean",
                                                 .reason = "renderer:paused",
                                                 .decoded_packets = 3,
                                                 .frames = 2,
                                             });
    mirage::receiver_client_media_status media;
    media.active = true;
    media.title = "track";
    media.artist = "artist";
    media.position_ms = 5000;
    media.duration_ms = 200000;
    tracker.client_media_updated(client_id, std::move(media));

    json = read_text(status_path);
    auto summary = mirage::parse_status_summary(json);
    ok &= expect(contains(json, "\"pid\":42"), "updated status pid missing");
    ok &= expect(summary.name == "Living Room", "parsed name mismatch");
    ok &= expect(summary.clients.size() == 1, "client count mismatch");
    if (!summary.clients.empty()) {
        ok &= expect(summary.clients.front().name == "cast", "default client name mismatch");
        ok &= expect(summary.clients.front().media.title == "track", "media title mismatch");
        ok &= expect(summary.clients.front().streams.size() == 2, "stream count mismatch");
        bool found_media_stream = false;
        for (const auto& stream : summary.clients.front().streams) {
            if (stream.kind == "media") {
                found_media_stream = true;
                ok &= expect(stream.reason == "renderer:paused", "renderer reason mismatch");
                ok &= expect(stream.decoded_packets.value_or(0) == 3,
                             "renderer decoded count mismatch");
                ok &= expect(stream.frames.value_or(0) == 2, "renderer frame count mismatch");
            }
        }
        ok &= expect(found_media_stream, "renderer stream missing");
    }

    tracker.client_disconnected(client_id);
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::listening,
        "adapter did not return to listening state");
    json = read_text(status_path);
    ok &= expect(contains(json, "\"clients\":[]"), "disconnect did not clear clients");

    std::filesystem::remove_all(root, ec);
    return ok ? 0 : 1;
}
