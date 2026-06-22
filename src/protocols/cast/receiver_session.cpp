#include <optional>
#include <utility>

#include "core/log.hpp"
#include "protocols/protocols.hpp"
#include "protocols/receiver_sessions.hpp"

namespace mirage::protocols {
namespace {

class cast_receiver_session final : public receiver_session {
public:
    cast_receiver_session(io::io_context& ctx, uint16_t port) : ctx_(ctx), port_(port) {}

    [[nodiscard]] protocol id() const override { return protocol::cast; }
    [[nodiscard]] uint16_t port() const override { return port_; }

    [[nodiscard]] receiver_session_capabilities capabilities() const override {
        return {
            .network_listener = true,
            .discovery = true,
            .pairing = false,
            .media_setup = true,
            .audio = true,
            .video = true,
            .remote_control = true,
            .metadata = true,
            .transport = "cast-v2",
        };
    }

    result<void> start(receiver_adapter_registry& adapters) override {
        auto receiver = cast_receiver::bind(ctx_, port_);
        if (!receiver) {
            adapters.mark_error(id(), receiver.error().message);
            return std::unexpected(receiver.error());
        }
        receiver_.emplace(std::move(*receiver));
        adapters.mark_listening(id());
        log::info("cast receiver on port {}", port_);
        return {};
    }

    io::task<void> run() override {
        if (receiver_) {
            co_await receiver_->run();
        }
    }

    void stop(receiver_adapter_registry& adapters) override {
        if (receiver_) {
            receiver_->stop();
            adapters.mark_stopped(id());
        }
    }

private:
    io::io_context& ctx_;
    uint16_t port_;
    std::optional<cast_receiver> receiver_;
};

}  // namespace

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx, uint16_t port) {
    return std::make_unique<cast_receiver_session>(ctx, port);
}

}  // namespace mirage::protocols
