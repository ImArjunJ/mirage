#include <optional>
#include <string>
#include <utility>

#include "core/log.hpp"
#include "discovery/discovery.hpp"
#include "protocols/protocols.hpp"
#include "protocols/receiver_sessions.hpp"

namespace mirage::protocols {
namespace {

class cast_receiver_session final : public receiver_session {
public:
    cast_receiver_session(io::io_context& ctx, receiver_source_descriptor source,
                          std::string device_name, protocol_receiver_identity identity)
        : ctx_(ctx),
          source_(source),
          device_name_(std::move(device_name)),
          identity_(std::move(identity)) {}

    [[nodiscard]] protocol id() const override { return source_.id; }
    [[nodiscard]] uint16_t port() const override { return source_.port; }

    [[nodiscard]] receiver_session_capabilities capabilities() const override {
        return source_.capabilities;
    }

    result<void> start(receiver_adapter_registry& adapters,
                       discovery::service_publisher& publisher) override {
        auto receiver = cast_receiver::bind(ctx_, source_.port, device_name_);
        if (!receiver) {
            adapters.mark_error(id(), receiver.error().message);
            return std::unexpected(receiver.error());
        }
        receiver_.emplace(std::move(*receiver));
        adapters.mark_listening(id());
        publish_discovery(adapters, publisher);
        log::diagnostic("Cast stream setup: mode=probe_only, port={}", source_.port);
        return {};
    }

    io::task<void> run() override {
        if (receiver_) {
            co_await receiver_->run();
        }
    }

    void stop(receiver_adapter_registry& adapters,
              discovery::service_publisher& publisher) override {
        publisher.withdraw(id());
        if (receiver_) {
            receiver_->stop();
            adapters.mark_stopped(id());
        }
    }

private:
    void publish_discovery(receiver_adapter_registry& adapters,
                           discovery::service_publisher& publisher) {
        if (!publisher.enabled()) {
            return;
        }

        auto service = discovery::create_cast_service(device_name_, source_.port, identity_);
        if (auto published = publisher.publish(id(), std::move(service)); !published) {
            log::error("failed to register cast service: {}", published.error().message);
            adapters.set_detail(id(), published.error().message);
            return;
        }
        adapters.mark_advertised(id());
        log::info("cast probe advertised on port {} (uuid: {})", source_.port, identity_.uuid);
    }

    io::io_context& ctx_;
    receiver_source_descriptor source_;
    std::string device_name_;
    protocol_receiver_identity identity_;
    std::optional<cast_receiver> receiver_;
};

}  // namespace

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx,
                                                             receiver_source_descriptor source,
                                                             std::string device_name,
                                                             protocol_receiver_identity identity) {
    return std::make_unique<cast_receiver_session>(ctx, source, std::move(device_name),
                                                   std::move(identity));
}

}  // namespace mirage::protocols
