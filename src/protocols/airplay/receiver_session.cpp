#include <array>
#include <optional>
#include <string>
#include <utility>

#include "core/log.hpp"
#include "discovery/discovery.hpp"
#include "protocols/protocols.hpp"
#include "protocols/receiver_sessions.hpp"

namespace mirage::protocols {
namespace {

class airplay_receiver_session final : public receiver_session {
public:
    airplay_receiver_session(io::io_context& ctx, uint16_t port, crypto::ed25519_keypair keypair,
                             std::string device_name, std::string mac_address)
        : ctx_(ctx),
          port_(port),
          keypair_(std::move(keypair)),
          public_key_(keypair_.public_key()),
          device_name_(std::move(device_name)),
          mac_address_(std::move(mac_address)) {}

    [[nodiscard]] protocol id() const override { return protocol::airplay; }
    [[nodiscard]] uint16_t port() const override { return port_; }

    [[nodiscard]] receiver_session_capabilities capabilities() const override {
        return {
            .network_listener = true,
            .discovery = true,
            .pairing = true,
            .media_setup = true,
            .audio = true,
            .video = true,
            .remote_control = true,
            .metadata = true,
            .transport = "rtsp/raop",
        };
    }

    result<void> start(receiver_adapter_registry& adapters,
                       discovery::service_publisher& publisher) override {
        auto server = rtsp_server::bind(ctx_, port_, std::move(keypair_));
        if (!server) {
            adapters.mark_error(id(), server.error().message);
            return std::unexpected(server.error());
        }
        rtsp_.emplace(std::move(*server));
        adapters.mark_listening(id());
        publish_discovery(adapters, publisher);
        log::info("rtsp server on port {}", port_);
        return {};
    }

    io::task<void> run() override {
        if (rtsp_) {
            co_await rtsp_->run();
        }
    }

    void stop(receiver_adapter_registry& adapters,
              discovery::service_publisher& publisher) override {
        publisher.withdraw(id());
        if (rtsp_) {
            rtsp_->stop();
            adapters.mark_stopped(id());
        }
    }

private:
    void publish_discovery(receiver_adapter_registry& adapters,
                           discovery::service_publisher& publisher) {
        if (!publisher.enabled()) {
            return;
        }

        size_t advertised_records = 0;
        auto airplay_service =
            discovery::create_airplay_service(device_name_, port_, public_key_, mac_address_);
        if (auto published = publisher.publish(id(), std::move(airplay_service)); !published) {
            log::error("failed to register airplay service: {}", published.error().message);
            adapters.set_detail(id(), published.error().message);
        } else {
            ++advertised_records;
        }

        auto raop_service = discovery::create_raop_service(device_name_, port_, mac_address_);
        if (auto published = publisher.publish(id(), std::move(raop_service)); !published) {
            log::error("failed to register raop service: {}", published.error().message);
            adapters.set_detail(id(), published.error().message);
        } else {
            ++advertised_records;
        }

        if (advertised_records > 0) {
            adapters.mark_advertised(id());
        }
    }

    io::io_context& ctx_;
    uint16_t port_;
    crypto::ed25519_keypair keypair_;
    std::array<std::byte, 32> public_key_;
    std::string device_name_;
    std::string mac_address_;
    std::optional<rtsp_server> rtsp_;
};

}  // namespace

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx, uint16_t port,
                                                                crypto::ed25519_keypair keypair,
                                                                std::string device_name,
                                                                std::string mac_address) {
    return std::make_unique<airplay_receiver_session>(
        ctx, port, std::move(keypair), std::move(device_name), std::move(mac_address));
}

}  // namespace mirage::protocols
