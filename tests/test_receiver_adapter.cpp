#include <iostream>
#include <string>
#include <vector>

#include "core/receiver_adapter.hpp"
#include "core/receiver_source.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    mirage::config cfg;
    mirage::receiver_adapter_registry adapters(cfg);
    bool ok = true;

    auto* airplay = adapters.find(mirage::protocol::airplay);
    auto* cast = adapters.find(mirage::protocol::cast);
    auto* miracast = adapters.find(mirage::protocol::miracast);

    ok &= expect(adapters.all().size() == 3, "adapter count mismatch");
    ok &= expect(airplay != nullptr, "airplay adapter missing");
    ok &= expect(cast != nullptr, "cast adapter missing");
    ok &= expect(miracast != nullptr, "miracast adapter missing");
    if (!ok) {
        return 1;
    }

    ok &= expect(airplay->state == mirage::receiver_adapter_state::unavailable,
                 "airplay initial state mismatch");
    ok &= expect(airplay->port == 7000, "airplay port mismatch");
    ok &= expect(cast->state == mirage::receiver_adapter_state::disabled,
                 "cast initial state mismatch");
    ok &= expect(miracast->state == mirage::receiver_adapter_state::disabled,
                 "miracast initial state mismatch");

    adapters.mark_listening(mirage::protocol::airplay);
    adapters.mark_advertised(mirage::protocol::airplay);
    ok &= expect(airplay->state == mirage::receiver_adapter_state::listening,
                 "airplay listening transition mismatch");
    ok &= expect(airplay->advertised, "airplay advertised transition mismatch");

    adapters.mark_error(mirage::protocol::cast, "bind failed");
    ok &= expect(cast->state == mirage::receiver_adapter_state::error,
                 "cast error transition mismatch");
    ok &= expect(cast->detail == std::string("bind failed"), "cast error detail mismatch");

    adapters.mark_stopped(mirage::protocol::airplay);
    ok &= expect(airplay->state == mirage::receiver_adapter_state::stopped,
                 "airplay stopped transition mismatch");

    mirage::receiver_source_registry source_registry({
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::airplay,
            .port = 7000,
            .enabled = true,
            .experimental = true,
            .detail = "rtsp/raop receiver",
            .capabilities = {.network_listener = true, .discovery = true, .transport = "rtsp"},
        },
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::cast,
            .port = 8009,
            .enabled = true,
            .experimental = true,
            .detail = "cast v2 receiver",
            .capabilities = {.network_listener = true, .discovery = true, .transport = "cast-v2"},
        },
    });
    mirage::receiver_adapter_registry source_adapters(source_registry.all());
    const auto* source_airplay = source_adapters.find(mirage::protocol::airplay);
    const auto* source_cast = source_adapters.find(mirage::protocol::cast);

    ok &= expect(source_registry.enabled().size() == 2, "enabled source count mismatch");
    ok &= expect(source_registry.find(mirage::protocol::miracast) == nullptr,
                 "unexpected miracast source");
    ok &= expect(source_airplay != nullptr, "source airplay adapter missing");
    ok &= expect(source_cast != nullptr, "source cast adapter missing");
    if (source_airplay != nullptr) {
        ok &= expect(source_airplay->state == mirage::receiver_adapter_state::unavailable,
                     "source airplay initial state mismatch");
        ok &= expect(source_airplay->detail == std::string("rtsp/raop receiver"),
                     "source airplay detail mismatch");
    }
    if (source_cast != nullptr) {
        ok &= expect(source_cast->port == 8009, "source cast port mismatch");
        ok &= expect(source_cast->detail == std::string("cast v2 receiver"),
                     "source cast detail mismatch");
    }

    return ok ? 0 : 1;
}
