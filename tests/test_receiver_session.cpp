#include <iostream>
#include <memory>

#include "core/receiver_session.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class fake_receiver_session final : public mirage::receiver_session {
public:
    [[nodiscard]] mirage::protocol id() const override { return mirage::protocol::cast; }
    [[nodiscard]] uint16_t port() const override { return 8009; }

    [[nodiscard]] mirage::receiver_session_capabilities capabilities() const override {
        return {
            .network_listener = true,
            .discovery = true,
            .pairing = false,
            .media_setup = true,
            .audio = true,
            .video = true,
            .remote_control = true,
            .metadata = true,
            .transport = "test",
        };
    }

    mirage::result<void> start(mirage::receiver_adapter_registry& adapters) override {
        adapters.mark_listening(id());
        started = true;
        return {};
    }

    mirage::io::task<void> run() override { co_return; }

    void stop(mirage::receiver_adapter_registry& adapters) override {
        adapters.mark_stopped(id());
        stopped = true;
    }

    bool started = false;
    bool stopped = false;
};

}  // namespace

int main() {
    mirage::config cfg;
    cfg.enable_cast = true;
    mirage::receiver_adapter_registry adapters(cfg);

    std::unique_ptr<mirage::receiver_session> session = std::make_unique<fake_receiver_session>();
    bool ok = true;

    ok &= expect(session->id() == mirage::protocol::cast, "session protocol mismatch");
    ok &= expect(session->port() == 8009, "session port mismatch");

    auto capabilities = session->capabilities();
    ok &= expect(capabilities.network_listener, "session listener capability mismatch");
    ok &= expect(capabilities.discovery, "session discovery capability mismatch");
    ok &= expect(capabilities.audio, "session audio capability mismatch");
    ok &= expect(capabilities.video, "session video capability mismatch");
    ok &= expect(capabilities.transport == "test", "session transport mismatch");

    auto started = session->start(adapters);
    ok &= expect(started.has_value(), "session start failed");
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::listening,
        "session did not update listening state");

    session->stop(adapters);
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::stopped,
        "session did not update stopped state");

    return ok ? 0 : 1;
}
