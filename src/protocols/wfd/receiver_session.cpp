#include <optional>
#include <utility>

#include "core/log.hpp"
#include "discovery/discovery.hpp"
#include "protocols/protocols.hpp"
#include "protocols/receiver_sessions.hpp"

namespace mirage::protocols {
namespace {

class wfd_receiver_session final : public receiver_session {
public:
    wfd_receiver_session(io::io_context& ctx, receiver_source_descriptor source,
                         receiver_session_observer* observer)
        : ctx_(ctx), source_(source), observer_(observer) {}

    [[nodiscard]] protocol id() const override { return source_.id; }
    [[nodiscard]] uint16_t port() const override { return source_.port; }

    [[nodiscard]] receiver_session_capabilities capabilities() const override {
        return source_.capabilities;
    }

    result<void> start(receiver_adapter_registry& adapters,
                       discovery::service_publisher& discovery) override {
        static_cast<void>(discovery);
        auto session = wfd_session::bind(ctx_, source_.port, observer_);
        if (!session) {
            adapters.mark_error(id(), session.error().message);
            return std::unexpected(session.error());
        }
        session_.emplace(std::move(*session));
        adapters.mark_listening(id());
        log::diagnostic("Miracast stream setup: mode=capability_listener, port={}",
                        source_.port);
        return {};
    }

    io::task<void> run() override {
        if (session_) {
            co_await session_->run();
        }
    }

    void stop(receiver_adapter_registry& adapters,
              discovery::service_publisher& discovery) override {
        discovery.withdraw(id());
        if (session_) {
            session_->stop();
            adapters.mark_stopped(id());
        }
    }

private:
    io::io_context& ctx_;
    receiver_source_descriptor source_;
    receiver_session_observer* observer_ = nullptr;
    std::optional<wfd_session> session_;
};

}  // namespace

std::unique_ptr<receiver_session> make_wfd_receiver_session(io::io_context& ctx,
                                                            receiver_source_descriptor source,
                                                            receiver_session_observer* observer) {
    return std::make_unique<wfd_receiver_session>(ctx, source, observer);
}

}  // namespace mirage::protocols
