#include <optional>
#include <utility>

#include "core/log.hpp"
#include "protocols/protocols.hpp"
#include "protocols/receiver_sessions.hpp"

namespace mirage::protocols {
namespace {

class airplay_receiver_session final : public receiver_session {
public:
    airplay_receiver_session(io::io_context& ctx, uint16_t port, crypto::ed25519_keypair keypair)
        : ctx_(ctx), port_(port), keypair_(std::move(keypair)) {}

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

    result<void> start(receiver_adapter_registry& adapters) override {
        auto server = rtsp_server::bind(ctx_, port_, std::move(keypair_));
        if (!server) {
            adapters.mark_error(id(), server.error().message);
            return std::unexpected(server.error());
        }
        rtsp_.emplace(std::move(*server));
        adapters.mark_listening(id());
        log::info("rtsp server on port {}", port_);
        return {};
    }

    io::task<void> run() override {
        if (rtsp_) {
            co_await rtsp_->run();
        }
    }

    void stop(receiver_adapter_registry& adapters) override {
        if (rtsp_) {
            rtsp_->stop();
            adapters.mark_stopped(id());
        }
    }

private:
    io::io_context& ctx_;
    uint16_t port_;
    crypto::ed25519_keypair keypair_;
    std::optional<rtsp_server> rtsp_;
};

}  // namespace

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx, uint16_t port,
                                                                crypto::ed25519_keypair keypair) {
    return std::make_unique<airplay_receiver_session>(ctx, port, std::move(keypair));
}

}  // namespace mirage::protocols
