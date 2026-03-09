#include "io/tcp.hpp"

#include <coroutine>
#include <system_error>

#include "io/event_loop.hpp"

#ifdef _WIN32
#include <mswsock.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mirage::io {

#ifdef _WIN32

namespace {

void set_nonblocking(socket_t fd) {
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
}

void close_socket(socket_t fd) {
    ::closesocket(fd);
}

endpoint endpoint_from_sockaddr(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET) {
        auto& sa = reinterpret_cast<const sockaddr_in&>(ss);
        uint32_t addr = ntohl(sa.sin_addr.s_addr);
        uint16_t port = ntohs(sa.sin_port);
        return {ip_address::v4(addr), port};
    }
    auto& sa6 = reinterpret_cast<const sockaddr_in6&>(ss);
    ip_address::v6_bytes bytes;
    std::memcpy(bytes.data(), &sa6.sin6_addr, 16);
    uint16_t port = ntohs(sa6.sin6_port);
    return {ip_address::v6(bytes, sa6.sin6_scope_id), port};
}

void throw_wsa(const char* what) {
    throw std::system_error(WSAGetLastError(), std::system_category(), what);
}

sockaddr_storage to_sockaddr(const endpoint& ep, socklen_t& len) {
    sockaddr_storage ss{};
    if (ep.addr.is_v4()) {
        auto& sa = reinterpret_cast<sockaddr_in&>(ss);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(ep.port);
        sa.sin_addr.s_addr = htonl(ep.addr.to_v4_uint());
        len = sizeof(sockaddr_in);
    } else {
        auto& sa6 = reinterpret_cast<sockaddr_in6&>(ss);
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(ep.port);
        auto bytes = ep.addr.to_v6_bytes();
        std::memcpy(&sa6.sin6_addr, bytes.data(), 16);
        sa6.sin6_scope_id = ep.addr.scope_id();
        len = sizeof(sockaddr_in6);
    }
    return ss;
}

struct io_operation {
    OVERLAPPED overlapped{};
    std::coroutine_handle<> handle;
};

struct read_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<std::byte> buf;
    io_operation op{};

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op.overlapped = {};
        op.handle = h;
        WSABUF wsabuf{};
        wsabuf.buf = reinterpret_cast<char*>(buf.data());
        wsabuf.len = static_cast<ULONG>(buf.size());
        DWORD flags = 0;
        int rc = WSARecv(fd, &wsabuf, 1, nullptr, &flags, &op.overlapped, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            ctx->post_completion(h);
        }
    }

    [[nodiscard]] size_t await_resume() {
        DWORD transferred = 0;
        DWORD flags = 0;
        BOOL ok = WSAGetOverlappedResult(fd, &op.overlapped, &transferred, FALSE, &flags);
        if (!ok) {
            throw_wsa("WSARecv");
        }
        if (transferred == 0) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        return static_cast<size_t>(transferred);
    }
};

struct write_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<const std::byte> buf;
    io_operation op{};

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op.overlapped = {};
        op.handle = h;
        WSABUF wsabuf{};
        wsabuf.buf = const_cast<char*>(reinterpret_cast<const char*>(buf.data()));
        wsabuf.len = static_cast<ULONG>(buf.size());
        int rc = WSASend(fd, &wsabuf, 1, nullptr, 0, &op.overlapped, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            ctx->post_completion(h);
        }
    }

    [[nodiscard]] size_t await_resume() {
        DWORD transferred = 0;
        DWORD flags = 0;
        BOOL ok = WSAGetOverlappedResult(fd, &op.overlapped, &transferred, FALSE, &flags);
        if (!ok) {
            throw_wsa("WSASend");
        }
        return static_cast<size_t>(transferred);
    }
};

struct accept_awaiter {
    io_context* ctx;
    socket_t listen_fd;
    socket_t accept_fd = invalid_socket_v;
    io_operation op{};
    char addr_buf[(sizeof(sockaddr_storage) + 16) * 2]{};

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op.overlapped = {};
        op.handle = h;
        accept_fd = WSASocketW(AF_INET, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (accept_fd == invalid_socket_v) {
            ctx->post_completion(h);
            return;
        }
        DWORD addr_len = sizeof(sockaddr_storage) + 16;
        DWORD bytes = 0;
        BOOL ok =
            AcceptEx(listen_fd, accept_fd, addr_buf, 0, addr_len, addr_len, &bytes, &op.overlapped);
        if (!ok && WSAGetLastError() != ERROR_IO_PENDING) {
            closesocket(accept_fd);
            accept_fd = invalid_socket_v;
            ctx->post_completion(h);
        }
    }

    [[nodiscard]] socket_t await_resume() {
        if (accept_fd == invalid_socket_v) {
            throw_wsa("AcceptEx");
        }
        setsockopt(accept_fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                   reinterpret_cast<const char*>(&listen_fd), sizeof(listen_fd));
        return accept_fd;
    }
};

}  // namespace

tcp_stream::tcp_stream(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {
    set_nonblocking(fd_);
    ctx_->associate(fd_);
}

tcp_stream::tcp_stream(tcp_stream&& other) noexcept
    : ctx_(other.ctx_),
      fd_(std::exchange(other.fd_, invalid_socket_v)),
      read_buffer_(std::move(other.read_buffer_)) {}

tcp_stream& tcp_stream::operator=(tcp_stream&& other) noexcept {
    if (this != &other) {
        if (fd_ != invalid_socket_v) {
            close();
        }
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
        read_buffer_ = std::move(other.read_buffer_);
    }
    return *this;
}

tcp_stream::~tcp_stream() {
    if (fd_ != invalid_socket_v) {
        close();
    }
}

void tcp_stream::close() {
    if (fd_ != invalid_socket_v) {
        CancelIoEx(reinterpret_cast<HANDLE>(fd_), nullptr);
        closesocket(fd_);
        fd_ = invalid_socket_v;
    }
}

task<size_t> tcp_stream::async_read(std::span<std::byte> buf) {
    co_return co_await read_awaiter{ctx_, fd_, buf};
}

task<void> tcp_stream::async_read_exactly(std::span<std::byte> buf) {
    size_t offset = 0;
    while (offset < buf.size()) {
        auto n = co_await async_read(buf.subspan(offset));
        offset += n;
    }
}

task<void> tcp_stream::async_write(std::span<const std::byte> data) {
    size_t offset = 0;
    while (offset < data.size()) {
        auto n = co_await write_awaiter{ctx_, fd_, data.subspan(offset)};
        offset += n;
    }
}

task<std::string> tcp_stream::async_read_until(std::string_view delim) {
    while (true) {
        auto pos = read_buffer_.find(delim);
        if (pos != std::string::npos) {
            auto end = pos + delim.size();
            std::string result = read_buffer_.substr(0, end);
            read_buffer_.erase(0, end);
            co_return result;
        }
        std::byte tmp[4096];
        auto n = co_await read_awaiter{ctx_, fd_, std::span<std::byte>(tmp)};
        read_buffer_.append(reinterpret_cast<const char*>(tmp), n);
    }
}

endpoint tcp_stream::local_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

endpoint tcp_stream::remote_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

bool tcp_stream::is_open() const {
    return fd_ != invalid_socket_v;
}

socket_t tcp_stream::native_handle() const {
    return fd_;
}

io_context& tcp_stream::context() const {
    return *ctx_;
}

std::string& tcp_stream::buffer() {
    return read_buffer_;
}

tcp_acceptor::tcp_acceptor(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {}

tcp_acceptor tcp_acceptor::bind(io_context& ctx, uint16_t port) {
    return bind(ctx, endpoint{ip_address::v4_any(), port});
}

tcp_acceptor tcp_acceptor::bind(io_context& ctx, endpoint ep) {
    int family = ep.addr.is_v4() ? AF_INET : AF_INET6;
    socket_t fd = WSASocketW(family, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (fd == invalid_socket_v) {
        throw_wsa("WSASocketW");
    }

    const char opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == SOCKET_ERROR) {
        closesocket(fd);
        throw_wsa("setsockopt");
    }

    set_nonblocking(fd);

    socklen_t sa_len = 0;
    auto ss = to_sockaddr(ep, sa_len);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&ss), sa_len) == SOCKET_ERROR) {
        closesocket(fd);
        throw_wsa("bind");
    }

    if (::listen(fd, 128) == SOCKET_ERROR) {
        closesocket(fd);
        throw_wsa("listen");
    }

    ctx.associate(fd);
    return tcp_acceptor(ctx, fd);
}

tcp_acceptor::tcp_acceptor(tcp_acceptor&& other) noexcept
    : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket_v)) {}

tcp_acceptor& tcp_acceptor::operator=(tcp_acceptor&& other) noexcept {
    if (this != &other) {
        if (fd_ != invalid_socket_v) {
            close();
        }
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
    }
    return *this;
}

tcp_acceptor::~tcp_acceptor() {
    if (fd_ != invalid_socket_v) {
        close();
    }
}

task<tcp_stream> tcp_acceptor::async_accept() {
    auto fd = co_await accept_awaiter{ctx_, fd_};
    co_return tcp_stream{*ctx_, fd};
}

endpoint tcp_acceptor::local_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

bool tcp_acceptor::is_open() const {
    return fd_ != invalid_socket_v;
}

io_context& tcp_acceptor::context() const {
    return *ctx_;
}

void tcp_acceptor::close() {
    if (fd_ != invalid_socket_v) {
        CancelIoEx(reinterpret_cast<HANDLE>(fd_), nullptr);
        closesocket(fd_);
        fd_ = invalid_socket_v;
    }
}

void tcp_acceptor::listen(int backlog) const {
    ::listen(fd_, backlog);
}

#else  // !_WIN32

namespace {

void set_nonblocking(socket_t fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_socket(socket_t fd) {
    ::close(fd);
}

endpoint endpoint_from_sockaddr(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET) {
        auto& sa = reinterpret_cast<const sockaddr_in&>(ss);
        uint32_t addr = ntohl(sa.sin_addr.s_addr);
        uint16_t port = ntohs(sa.sin_port);
        return {ip_address::v4(addr), port};
    }
    auto& sa6 = reinterpret_cast<const sockaddr_in6&>(ss);
    ip_address::v6_bytes bytes;
    std::memcpy(bytes.data(), &sa6.sin6_addr, 16);
    uint16_t port = ntohs(sa6.sin6_port);
    return {ip_address::v6(bytes, sa6.sin6_scope_id), port};
}

void throw_errno(const char* what) {
    throw std::system_error(errno, std::system_category(), what);
}

struct read_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<std::byte> buf;

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        ctx->watch_read(static_cast<int>(fd), h);
    }

    [[nodiscard]] size_t await_resume() const {
        auto n = ::recv(fd, reinterpret_cast<char*>(buf.data()), buf.size(), 0);
        if (n < 0) {
            auto saved_errno = errno;
            throw std::system_error(saved_errno, std::system_category(), "recv");
        }
        if (n == 0) {
            throw std::system_error(std::make_error_code(std::errc::connection_reset));
        }
        return static_cast<size_t>(n);
    }
};

struct write_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<const std::byte> buf;

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        ctx->watch_write(static_cast<int>(fd), h);
    }

    [[nodiscard]] size_t await_resume() const {
        int flags = MSG_NOSIGNAL;
        auto n = ::send(fd, reinterpret_cast<const char*>(buf.data()), buf.size(), flags);
        if (n < 0) {
            throw std::system_error(errno, std::system_category(), "send");
        }
        return static_cast<size_t>(n);
    }
};

struct accept_awaiter {
    io_context* ctx;
    socket_t listen_fd;

    [[nodiscard]] bool await_ready() const { return false; }

    void await_suspend(std::coroutine_handle<> h) const {
        ctx->watch_read(static_cast<int>(listen_fd), h);
    }

    [[nodiscard]] socket_t await_resume() const {
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        socket_t fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd == invalid_socket_v) {
            throw std::system_error(errno, std::system_category(), "accept");
        }
        return fd;
    }
};

sockaddr_storage to_sockaddr(const endpoint& ep, socklen_t& len) {
    sockaddr_storage ss{};
    if (ep.addr.is_v4()) {
        auto& sa = reinterpret_cast<sockaddr_in&>(ss);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(ep.port);
        sa.sin_addr.s_addr = htonl(ep.addr.to_v4_uint());
        len = sizeof(sockaddr_in);
    } else {
        auto& sa6 = reinterpret_cast<sockaddr_in6&>(ss);
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(ep.port);
        auto bytes = ep.addr.to_v6_bytes();
        std::memcpy(&sa6.sin6_addr, bytes.data(), 16);
        sa6.sin6_scope_id = ep.addr.scope_id();
        len = sizeof(sockaddr_in6);
    }
    return ss;
}

}  // namespace

tcp_stream::tcp_stream(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {
    set_nonblocking(fd_);
}

tcp_stream::tcp_stream(tcp_stream&& other) noexcept
    : ctx_(other.ctx_),
      fd_(std::exchange(other.fd_, invalid_socket_v)),
      read_buffer_(std::move(other.read_buffer_)) {}

tcp_stream& tcp_stream::operator=(tcp_stream&& other) noexcept {
    if (this != &other) {
        if (fd_ != invalid_socket_v) {
            close();
        }
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
        read_buffer_ = std::move(other.read_buffer_);
    }
    return *this;
}

tcp_stream::~tcp_stream() {
    if (fd_ != invalid_socket_v) {
        close();
    }
}

void tcp_stream::close() {
    if (fd_ != invalid_socket_v) {
        ctx_->cancel(static_cast<int>(fd_));
        close_socket(fd_);
        fd_ = invalid_socket_v;
    }
}

task<size_t> tcp_stream::async_read(std::span<std::byte> buf) {
    co_return co_await read_awaiter{ctx_, fd_, buf};
}

task<void> tcp_stream::async_read_exactly(std::span<std::byte> buf) {
    size_t offset = 0;
    while (offset < buf.size()) {
        auto n = co_await async_read(buf.subspan(offset));
        offset += n;
    }
}

task<void> tcp_stream::async_write(std::span<const std::byte> data) {
    size_t offset = 0;
    while (offset < data.size()) {
        auto n = co_await write_awaiter{ctx_, fd_, data.subspan(offset)};
        offset += n;
    }
}

task<std::string> tcp_stream::async_read_until(std::string_view delim) {
    while (true) {
        auto pos = read_buffer_.find(delim);
        if (pos != std::string::npos) {
            auto end = pos + delim.size();
            std::string result = read_buffer_.substr(0, end);
            read_buffer_.erase(0, end);
            co_return result;
        }
        std::byte tmp[4096];
        auto n = co_await read_awaiter{ctx_, fd_, std::span<std::byte>(tmp)};
        read_buffer_.append(reinterpret_cast<const char*>(tmp), n);
    }
}

endpoint tcp_stream::local_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    ::getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

endpoint tcp_stream::remote_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    ::getpeername(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

bool tcp_stream::is_open() const {
    return fd_ != invalid_socket_v;
}

socket_t tcp_stream::native_handle() const {
    return fd_;
}

io_context& tcp_stream::context() const {
    return *ctx_;
}

std::string& tcp_stream::buffer() {
    return read_buffer_;
}

tcp_acceptor::tcp_acceptor(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {}

tcp_acceptor tcp_acceptor::bind(io_context& ctx, uint16_t port) {
    return bind(ctx, endpoint{ip_address::v4_any(), port});
}

tcp_acceptor tcp_acceptor::bind(io_context& ctx, endpoint ep) {
    int family = ep.addr.is_v4() ? AF_INET : AF_INET6;
    socket_t fd = ::socket(family, SOCK_STREAM, 0);
    if (fd == invalid_socket_v) {
        throw_errno("socket");
    }

    int opt = 1;
    if (::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close_socket(fd);
        throw_errno("setsockopt");
    }

    set_nonblocking(fd);

    socklen_t sa_len = 0;
    auto ss = to_sockaddr(ep, sa_len);

    if (::bind(static_cast<int>(fd), reinterpret_cast<const sockaddr*>(&ss), sa_len) < 0) {
        close_socket(fd);
        throw_errno("bind");
    }

    if (::listen(static_cast<int>(fd), 128) < 0) {
        close_socket(fd);
        throw_errno("listen");
    }

    return tcp_acceptor(ctx, fd);
}

tcp_acceptor::tcp_acceptor(tcp_acceptor&& other) noexcept
    : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket_v)) {}

tcp_acceptor& tcp_acceptor::operator=(tcp_acceptor&& other) noexcept {
    if (this != &other) {
        if (fd_ != invalid_socket_v) {
            close();
        }
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
    }
    return *this;
}

tcp_acceptor::~tcp_acceptor() {
    if (fd_ != invalid_socket_v) {
        close();
    }
}

task<tcp_stream> tcp_acceptor::async_accept() {
    auto fd = co_await accept_awaiter{ctx_, fd_};
    co_return tcp_stream{*ctx_, fd};
}

endpoint tcp_acceptor::local_endpoint() const {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    ::getsockname(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&ss), &len);
    return endpoint_from_sockaddr(ss);
}

bool tcp_acceptor::is_open() const {
    return fd_ != invalid_socket_v;
}

io_context& tcp_acceptor::context() const {
    return *ctx_;
}

void tcp_acceptor::close() {
    if (fd_ != invalid_socket_v) {
        ctx_->cancel(static_cast<int>(fd_));
        close_socket(fd_);
        fd_ = invalid_socket_v;
    }
}

void tcp_acceptor::listen(int backlog) const {
    ::listen(static_cast<int>(fd_), backlog);
}

#endif  // !_WIN32

}  // namespace mirage::io
