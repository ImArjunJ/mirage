#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "core/receiver_session.hpp"
#include "discovery/discovery.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class recording_publisher final : public mirage::discovery::service_publisher {
public:
    [[nodiscard]] bool enabled() const override { return enabled_; }

    mirage::result<void> publish(mirage::protocol owner,
                                 mirage::discovery::service_record record) override {
        published_owner = owner;
        published_records.push_back(std::move(record));
        return {};
    }

    void withdraw(mirage::protocol owner) override { withdrawn_owner = owner; }

    void withdraw_all() override { withdraw_all_called = true; }

    bool enabled_ = true;
    bool withdraw_all_called = false;
    std::optional<mirage::protocol> published_owner;
    std::optional<mirage::protocol> withdrawn_owner;
    std::vector<mirage::discovery::service_record> published_records;
};

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

    mirage::result<void> start(mirage::receiver_adapter_registry& adapters,
                               mirage::discovery::service_publisher& discovery) override {
        adapters.mark_listening(id());
        if (discovery.enabled()) {
            auto published = discovery.publish(id(), {.name = "fake",
                                                      .service_type = "_fake._tcp",
                                                      .domain = "local",
                                                      .port = port(),
                                                      .txt_records = {{"mode", "test"}}});
            if (!published) {
                return std::unexpected(published.error());
            }
            adapters.mark_advertised(id());
        }
        started = true;
        return {};
    }

    mirage::io::task<void> run() override { co_return; }

    void stop(mirage::receiver_adapter_registry& adapters,
              mirage::discovery::service_publisher& discovery) override {
        discovery.withdraw(id());
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
    recording_publisher discovery;

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

    auto started = session->start(adapters, discovery);
    ok &= expect(started.has_value(), "session start failed");
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::listening,
        "session did not update listening state");
    ok &= expect(adapters.find(mirage::protocol::cast)->advertised,
                 "session did not mark adapter advertised");
    ok &= expect(discovery.published_owner == mirage::protocol::cast,
                 "session did not publish discovery under its protocol");
    ok &=
        expect(discovery.published_records.size() == 1, "session discovery record count mismatch");
    ok &= expect(!discovery.published_records.empty() &&
                     discovery.published_records.front().service_type == "_fake._tcp",
                 "session discovery record type mismatch");

    session->stop(adapters, discovery);
    ok &= expect(
        adapters.find(mirage::protocol::cast)->state == mirage::receiver_adapter_state::stopped,
        "session did not update stopped state");
    ok &= expect(discovery.withdrawn_owner == mirage::protocol::cast,
                 "session did not withdraw discovery records");

    return ok ? 0 : 1;
}
