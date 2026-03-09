#include <memory>

#include "core/core.hpp"
#include "core/log.hpp"
#include "io/io.hpp"
#include "protocols/protocols.hpp"
namespace mirage::protocols {
struct wfd_session::impl {
    io::io_context* ctx;
    bool running = false;
    explicit impl(io::io_context& context) : ctx(&context) {}
};
wfd_session::wfd_session(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
wfd_session::~wfd_session() = default;
wfd_session::wfd_session(wfd_session&&) noexcept = default;
wfd_session& wfd_session::operator=(wfd_session&&) noexcept = default;
result<wfd_session> wfd_session::create(io::io_context& ctx) {
    try {
        auto impl_ptr = std::make_unique<impl>(ctx);
        mirage::log::info("WFD (Miracast) session handler created");
        return wfd_session{std::move(impl_ptr)};
    } catch (const std::exception& e) {
        return std::unexpected(
            mirage_error::network(std::format("failed to create WFD session: {}", e.what())));
    }
}
io::task<void> wfd_session::run() {
    impl_->running = true;
    mirage::log::info("WFD session handler started (stub)");
    while (impl_->running) {
        io::steady_timer timer(*(impl_->ctx));
        timer.expires_after(std::chrono::seconds(1));
        co_await timer.async_wait();
    }
}
void wfd_session::stop() {
    impl_->running = false;
}
}  // namespace mirage::protocols
