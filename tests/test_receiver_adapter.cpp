#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/receiver_adapter.hpp"
#include "core/receiver_session.hpp"
#include "core/receiver_source.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contract_validator_called = false;
bool contract_factory_called = false;

class contract_receiver_session final : public mirage::receiver_session {
public:
    explicit contract_receiver_session(mirage::receiver_source_descriptor source)
        : source_(source) {}

    [[nodiscard]] mirage::protocol id() const override { return source_.id; }
    [[nodiscard]] uint16_t port() const override { return source_.port; }

    [[nodiscard]] mirage::receiver_session_capabilities capabilities() const override {
        return source_.capabilities;
    }

    mirage::result<void> start(mirage::receiver_adapter_registry& adapters,
                               mirage::discovery::service_publisher& discovery) override {
        static_cast<void>(discovery);
        adapters.mark_running(id());
        return {};
    }

    mirage::io::task<void> run() override { co_return; }

    void stop(mirage::receiver_adapter_registry& adapters,
              mirage::discovery::service_publisher& discovery) override {
        static_cast<void>(discovery);
        adapters.mark_stopped(id());
    }

private:
    mirage::receiver_source_descriptor source_;
};

mirage::result<void> validate_contract_source(const mirage::receiver_source_descriptor& source,
                                              const mirage::receiver_source_runtime& runtime) {
    contract_validator_called = true;
    if (source.id != mirage::protocol::cast) {
        return std::unexpected(mirage::mirage_error::session("unexpected source id"));
    }
    if (runtime.device_name != "unit") {
        return std::unexpected(mirage::mirage_error::session("unexpected device name"));
    }
    return {};
}

mirage::result<std::unique_ptr<mirage::receiver_session>> create_contract_session(
    const mirage::receiver_source_descriptor& source,
    const mirage::receiver_source_runtime& runtime) {
    static_cast<void>(runtime);
    contract_factory_called = true;
    return std::make_unique<contract_receiver_session>(source);
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
    ok &= expect(!cast->advertised, "cast error did not clear advertised state");

    adapters.mark_error(mirage::protocol::airplay, "bind failed");
    ok &= expect(airplay->state == mirage::receiver_adapter_state::error,
                 "airplay error transition mismatch");
    ok &= expect(!airplay->advertised, "airplay error did not clear advertised state");
    adapters.mark_listening(mirage::protocol::airplay);
    adapters.mark_advertised(mirage::protocol::airplay);
    ok &= expect(airplay->detail == std::string("rtsp/raop receiver"),
                 "airplay healthy transition did not restore detail");
    adapters.mark_stopped(mirage::protocol::airplay);
    ok &= expect(airplay->state == mirage::receiver_adapter_state::stopped,
                 "airplay stopped transition mismatch");
    ok &= expect(!airplay->advertised, "airplay stopped transition did not clear advertised state");

    mirage::receiver_source_registry source_registry({
        mirage::receiver_source_descriptor{
            .id = mirage::protocol::airplay,
            .port = 7000,
            .enabled = true,
            .experimental = false,
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

    ok &= expect(mirage::classify_audio_stream({
                     .decoded_packets = 10,
                     .silent_or_marker = 1,
                 }) == mirage::receiver_stream_health::clean,
                 "clean audio health mismatch");
    ok &= expect(mirage::classify_audio_stream({
                     .decoded_packets = 10,
                     .invalid = 1,
                 }) == mirage::receiver_stream_health::attention,
                 "attention audio health mismatch");
    ok &= expect(mirage::classify_video_stream({
                     .frames = 10,
                     .keyframes = 1,
                 }) == mirage::receiver_stream_health::clean,
                 "clean video health mismatch");
    ok &= expect(mirage::classify_video_stream({
                     .frames = 10,
                 }) == mirage::receiver_stream_health::attention,
                 "attention video health mismatch");

    mirage::receiver_source_descriptor contract_source{
        .id = mirage::protocol::cast,
        .port = 9000,
        .enabled = true,
        .experimental = true,
        .detail = "contract receiver",
        .capabilities = {.network_listener = true, .discovery = true, .transport = "contract"},
        .validate_source = validate_contract_source,
        .session_factory = create_contract_session,
    };
    mirage::receiver_source_runtime contract_runtime{
        .device_name = "unit",
        .mac_address = "AA:BB:CC:DD:EE:FF",
    };

    auto validation = contract_source.validate(contract_runtime);
    ok &= expect(validation.has_value(), "source validation failed");
    ok &= expect(contract_validator_called, "source validator was not called");

    contract_validator_called = false;
    contract_factory_called = false;
    auto contract_session = contract_source.create_session(contract_runtime);
    ok &= expect(contract_session.has_value(), "source session creation failed");
    ok &= expect(contract_validator_called, "source create did not validate first");
    ok &= expect(contract_factory_called, "source factory was not called");
    if (contract_session) {
        ok &= expect((*contract_session)->id() == mirage::protocol::cast,
                     "source session id mismatch");
        ok &= expect((*contract_session)->port() == 9000, "source session port mismatch");
        ok &= expect((*contract_session)->capabilities().transport == "contract",
                     "source session capability mismatch");
    }

    auto disabled_source = contract_source;
    disabled_source.enabled = false;
    ok &= expect(!disabled_source.create_session(contract_runtime),
                 "disabled source unexpectedly created a session");

    auto incomplete_source = contract_source;
    incomplete_source.session_factory = nullptr;
    ok &= expect(!incomplete_source.create_session(contract_runtime),
                 "source without factory unexpectedly created a session");

    return ok ? 0 : 1;
}
