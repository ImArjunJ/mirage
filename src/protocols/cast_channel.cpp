#include <cstdint>
#include <memory>

#include "core/core.hpp"
#include "core/log.hpp"
#include "io/io.hpp"
#include "protocols/protocols.hpp"
namespace mirage::protocols {
struct cast_receiver::impl {
    io::tcp_acceptor acceptor;
    bool running = false;
    impl(io::io_context& ctx, uint16_t port) : acceptor(io::tcp_acceptor::bind(ctx, port)) {}
};
cast_receiver::cast_receiver(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
cast_receiver::~cast_receiver() = default;
cast_receiver::cast_receiver(cast_receiver&&) noexcept = default;
cast_receiver& cast_receiver::operator=(cast_receiver&&) noexcept = default;
result<cast_receiver> cast_receiver::bind(io::io_context& ctx, uint16_t port) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx, port);
        mirage::log::info("Cast receiver bound to port {}", port);
        return cast_receiver{std::move(impl_ptr)};
    } catch (const std::exception& e) {
        return std::unexpected(
            mirage_error::network(std::format("failed to bind Cast receiver: {}", e.what())));
    }
}
io::task<void> cast_receiver::run() {
    impl_->running = true;
    while (impl_->running) {
        try {
            auto socket = co_await impl_->acceptor.async_accept();
            mirage::log::info("Cast connection from {}", socket.remote_endpoint().addr.to_string());
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::operation_canceled) {
                mirage::log::warn("Cast accept error: {}", e.what());
            }
        }
    }
}
void cast_receiver::stop() {
    impl_->running = false;
    impl_->acceptor.close();
}
}  // namespace mirage::protocols
