#include "io/udp.hpp"

#include <coroutine>
#include <cstring>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
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

void throw_wsa(const char* what) {
    throw std::system_error(WSAGetLastError(), std::system_category(), what);
}

endpoint endpoint_from_sockaddr(const sockaddr_in& sa) {
    uint32_t addr = ntohl(sa.sin_addr.s_addr);
    uint16_t port = ntohs(sa.sin_port);
    return {ip_address::v4(addr), port};
}

sockaddr_in sockaddr_from_endpoint(const endpoint& ep) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ep.port);
    sa.sin_addr.s_addr = htonl(ep.addr.to_v4_uint());
    return sa;
}

struct io_operation {
    OVERLAPPED overlapped{};
    std::coroutine_handle<> handle;
};

struct recv_from_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<std::byte> buf;
    endpoint* sender;
    io_operation op{};
    sockaddr_in from_addr{};
    INT from_len = sizeof(sockaddr_in);

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op.overlapped = {};
        op.handle = h;
        WSABUF wsabuf{};
        wsabuf.buf = reinterpret_cast<char*>(buf.data());
        wsabuf.len = static_cast<ULONG>(buf.size());
        DWORD flags = 0;
        from_len = sizeof(sockaddr_in);
        int rc =
            WSARecvFrom(fd, &wsabuf, 1, nullptr, &flags, reinterpret_cast<sockaddr*>(&from_addr),
                        &from_len, &op.overlapped, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            ctx->post_completion(h);
        }
    }

    [[nodiscard]] size_t await_resume() {
        DWORD transferred = 0;
        DWORD flags = 0;
        BOOL ok = WSAGetOverlappedResult(fd, &op.overlapped, &transferred, FALSE, &flags);
        if (!ok) {
            throw_wsa("WSARecvFrom");
        }
        *sender = endpoint_from_sockaddr(from_addr);
        return static_cast<size_t>(transferred);
    }
};

struct send_to_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<const std::byte> data;
    endpoint dest;
    io_operation op{};

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        op.overlapped = {};
        op.handle = h;
        WSABUF wsabuf{};
        wsabuf.buf = const_cast<char*>(reinterpret_cast<const char*>(data.data()));
        wsabuf.len = static_cast<ULONG>(data.size());
        auto sa = sockaddr_from_endpoint(dest);
        int rc = WSASendTo(fd, &wsabuf, 1, nullptr, 0, reinterpret_cast<const sockaddr*>(&sa),
                           sizeof(sa), &op.overlapped, nullptr);
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
            ctx->post_completion(h);
        }
    }

    [[nodiscard]] size_t await_resume() {
        DWORD transferred = 0;
        DWORD flags = 0;
        BOOL ok = WSAGetOverlappedResult(fd, &op.overlapped, &transferred, FALSE, &flags);
        if (!ok) {
            throw_wsa("WSASendTo");
        }
        return static_cast<size_t>(transferred);
    }
};

}  // namespace

udp_socket::udp_socket(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {}

udp_socket::udp_socket(udp_socket&& other) noexcept
    : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket_v)) {}

udp_socket& udp_socket::operator=(udp_socket&& other) noexcept {
    if (this != &other) {
        close();
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
    }
    return *this;
}

udp_socket::~udp_socket() {
    close();
}

udp_socket udp_socket::bind(io_context& ctx, uint16_t port) {
    socket_t fd = WSASocketW(AF_INET, SOCK_DGRAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (fd == invalid_socket_v) {
        throw_wsa("WSASocketW");
    }

    if (port != 0) {
        const char opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    set_nonblocking(fd);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        closesocket(fd);
        throw_wsa("bind");
    }

    ctx.associate(fd);
    return udp_socket(ctx, fd);
}

void udp_socket::close() {
    if (fd_ != invalid_socket_v) {
        CancelIoEx(reinterpret_cast<HANDLE>(fd_), nullptr);
        closesocket(fd_);
        fd_ = invalid_socket_v;
    }
}

task<size_t> udp_socket::async_recv_from(std::span<std::byte> buf, endpoint& sender) {
    co_return co_await recv_from_awaiter{ctx_, fd_, buf, &sender};
}

task<size_t> udp_socket::async_send_to(std::span<const std::byte> data, const endpoint& dest) {
    co_return co_await send_to_awaiter{ctx_, fd_, data, dest};
}

void udp_socket::set_multicast_interface(const ip_address& addr) {
    in_addr ia{};
    ia.s_addr = htonl(addr.to_v4_uint());
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ia), sizeof(ia));
}

void udp_socket::join_multicast(const ip_address& group) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = htonl(group.to_v4_uint());
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
               sizeof(mreq));
}

void udp_socket::set_multicast_loopback(bool enable) {
    const char val = enable ? 1 : 0;
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));
}

void udp_socket::set_reuse_address(bool enable) {
    const char val = enable ? 1 : 0;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
}

endpoint udp_socket::local_endpoint() const {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    return endpoint_from_sockaddr(sa);
}

bool udp_socket::is_open() const {
    return fd_ != invalid_socket_v;
}

socket_t udp_socket::native_handle() const {
    return fd_;
}

io_context& udp_socket::context() const {
    return *ctx_;
}

#else  // !_WIN32

namespace {

void set_nonblocking(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void close_socket(socket_t fd) {
    ::close(fd);
}

endpoint endpoint_from_sockaddr(const sockaddr_in& sa) {
    uint32_t addr = ntohl(sa.sin_addr.s_addr);
    uint16_t port = ntohs(sa.sin_port);
    return {ip_address::v4(addr), port};
}

sockaddr_in sockaddr_from_endpoint(const endpoint& ep) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(ep.port);
    sa.sin_addr.s_addr = htonl(ep.addr.to_v4_uint());
    return sa;
}

struct recv_from_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<std::byte> buf;
    endpoint* sender;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const { ctx->watch_read(fd, h); }

    [[nodiscard]] size_t await_resume() const {
        sockaddr_in sa{};
        socklen_t len = sizeof(sa);
        auto n = ::recvfrom(fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&sa), &len);
        if (n < 0) {
            throw std::system_error(errno, std::system_category(), "recvfrom");
        }
        *sender = endpoint_from_sockaddr(sa);
        return static_cast<size_t>(n);
    }
};

struct send_to_awaiter {
    io_context* ctx;
    socket_t fd;
    std::span<const std::byte> data;
    endpoint dest;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) const { ctx->watch_write(fd, h); }

    [[nodiscard]] size_t await_resume() const {
        auto sa = sockaddr_from_endpoint(dest);
        auto n = ::sendto(fd, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                          sizeof(sa));
        if (n < 0) {
            throw std::system_error(errno, std::system_category(), "sendto");
        }
        return static_cast<size_t>(n);
    }
};

}  // namespace

udp_socket::udp_socket(io_context& ctx, socket_t fd) : ctx_(&ctx), fd_(fd) {}

udp_socket::udp_socket(udp_socket&& other) noexcept
    : ctx_(other.ctx_), fd_(std::exchange(other.fd_, invalid_socket_v)) {}

udp_socket& udp_socket::operator=(udp_socket&& other) noexcept {
    if (this != &other) {
        close();
        ctx_ = other.ctx_;
        fd_ = std::exchange(other.fd_, invalid_socket_v);
    }
    return *this;
}

udp_socket::~udp_socket() {
    close();
}

udp_socket udp_socket::bind(io_context& ctx, uint16_t port) {
    socket_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == invalid_socket_v) {
        throw std::system_error(errno, std::system_category(), "socket");
    }

    if (port != 0) {
        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    set_nonblocking(fd);

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = INADDR_ANY;

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0) {
        close_socket(fd);
        throw std::system_error(errno, std::system_category(), "bind");
    }

    return udp_socket(ctx, fd);
}

void udp_socket::close() {
    if (fd_ != invalid_socket_v) {
        ctx_->cancel(fd_);
        close_socket(fd_);
        fd_ = invalid_socket_v;
    }
}

task<size_t> udp_socket::async_recv_from(std::span<std::byte> buf, endpoint& sender) {
    co_return co_await recv_from_awaiter{ctx_, fd_, buf, &sender};
}

task<size_t> udp_socket::async_send_to(std::span<const std::byte> data, const endpoint& dest) {
    co_return co_await send_to_awaiter{ctx_, fd_, data, dest};
}

void udp_socket::set_multicast_interface(const ip_address& addr) {
    in_addr ia{};
    ia.s_addr = htonl(addr.to_v4_uint());
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ia, sizeof(ia));
}

void udp_socket::join_multicast(const ip_address& group) {
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = htonl(group.to_v4_uint());
    mreq.imr_interface.s_addr = INADDR_ANY;
    ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
}

void udp_socket::set_multicast_loopback(bool enable) {
    int val = enable ? 1 : 0;
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));
}

void udp_socket::set_reuse_address(bool enable) {
    int val = enable ? 1 : 0;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
}

endpoint udp_socket::local_endpoint() const {
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
    return endpoint_from_sockaddr(sa);
}

bool udp_socket::is_open() const {
    return fd_ != invalid_socket_v;
}

socket_t udp_socket::native_handle() const {
    return fd_;
}

io_context& udp_socket::context() const {
    return *ctx_;
}

#endif  // !_WIN32

}  // namespace mirage::io
