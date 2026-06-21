#include <iostream>
#include <string>

#include "core/receiver_adapter.hpp"

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

    return ok ? 0 : 1;
}
