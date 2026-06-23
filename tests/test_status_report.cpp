#include <array>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

#include "core/receiver_adapter.hpp"
#include "core/receiver_client.hpp"
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

}  // namespace

int main() {
    bool ok = true;

    std::array sources{
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::airplay,
            .port = 7000,
            .enabled = true,
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
                    .app_lifecycle = false,
                    .media_control = true,
                    .metadata = true,
                    .transport = "rtsp/raop",
                },
        },
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::cast,
            .port = 8009,
            .enabled = true,
            .experimental = true,
            .detail = "cast v2 control/status receiver",
            .capabilities =
                {
                    .network_listener = true,
                    .discovery = true,
                    .remote_control = true,
                    .app_lifecycle = true,
                    .transport = "cast-v2",
                },
        },
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::miracast,
            .port = 7236,
            .enabled = false,
            .experimental = true,
            .detail = "wfd capability listener",
            .capabilities = {.network_listener = true, .transport = "wfd"},
        },
    };

    mirage::receiver_adapter_registry adapters(std::span<const mirage::receiver_source_descriptor>(
        sources.data(), sources.size()));
    adapters.mark_listening(mirage::protocol::airplay);
    adapters.mark_advertised(mirage::protocol::airplay);
    adapters.mark_error(mirage::protocol::cast, "bind \"failed\"");

    std::array clients{
        mirage::receiver_client_status{
            .id = 7,
            .protocol_id = mirage::protocol::airplay,
            .name = "Junie",
            .address = "192.0.2.20",
            .state = "connected",
            .connected_at = 123460,
        },
    };

    const auto json = mirage::render_status_json({
        .pid = 42,
        .name = "Living \"Room\"\n",
        .ip = "192.0.2.10",
        .interface_name = "wlan0",
        .identity_key = "/tmp/mirage/key",
        .airplay_port = 7000,
        .cast_port = 8009,
        .miracast_port = 7236,
        .started = 123456,
        .adapters = adapters.all(),
        .sources = sources,
        .clients = clients,
    });

    ok &= expect(contains(json, "\"pid\":42"), "pid missing");
    ok &= expect(contains(json, "\"name\":\"Living \\\"Room\\\"\\n\""),
                 "escaped name mismatch");
    ok &= expect(contains(json, "\"identity_key\":\"/tmp/mirage/key\""),
                 "identity key missing");
    ok &= expect(contains(json, "\"airplay_port\":7000"), "airplay port missing");
    ok &= expect(contains(json, "\"cast_port\":8009"), "cast port missing");
    ok &= expect(contains(json, "\"miracast_port\":7236"), "miracast port missing");
    ok &= expect(contains(json, "\"started\":123456"), "started time missing");
    ok &= expect(contains(json, "\"id\":\"airplay\""), "airplay protocol missing");
    ok &= expect(contains(json, "\"state\":\"listening\""), "airplay state missing");
    ok &= expect(contains(json, "\"advertised\":true"), "advertised state missing");
    ok &= expect(contains(json, "\"transport\":\"rtsp/raop\""), "airplay transport missing");
    ok &= expect(contains(json, "\"audio\":true"), "audio capability missing");
    ok &= expect(contains(json, "\"media_control\":true"), "media control capability missing");
    ok &= expect(contains(json, "\"metadata\":true"), "metadata capability missing");
    ok &= expect(contains(json, "\"id\":\"cast\""), "cast protocol missing");
    ok &= expect(contains(json, "\"app_lifecycle\":true"), "app lifecycle capability missing");
    ok &= expect(contains(json, "\"state\":\"error\""), "cast error state missing");
    ok &= expect(contains(json, "\"detail\":\"bind \\\"failed\\\"\""),
                 "escaped error detail missing");
    ok &= expect(contains(json, "\"id\":\"miracast\""), "miracast protocol missing");
    ok &= expect(contains(json, "\"state\":\"disabled\""), "disabled state missing");
    ok &= expect(contains(json, "\"clients\":[{"), "clients list missing");
    ok &= expect(contains(json, "\"id\":7"), "client id missing");
    ok &= expect(contains(json, "\"protocol\":\"airplay\""), "client protocol missing");
    ok &= expect(contains(json, "\"name\":\"Junie\""), "client name missing");
    ok &= expect(contains(json, "\"address\":\"192.0.2.20\""), "client address missing");
    ok &= expect(contains(json, "\"connected_at\":123460"), "client connected time missing");

    return ok ? 0 : 1;
}
