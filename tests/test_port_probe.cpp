#include <cstdint>
#include <iostream>
#include <optional>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "core/port_probe.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

#ifdef _WIN32
using socket_handle = SOCKET;

constexpr socket_handle invalid_socket_handle = INVALID_SOCKET;

void close_socket(socket_handle fd) {
    closesocket(fd);
}

class socket_runtime {
public:
    socket_runtime() : result_(WSAStartup(MAKEWORD(2, 2), &data_)) {}
    socket_runtime(const socket_runtime&) = delete;
    socket_runtime& operator=(const socket_runtime&) = delete;
    ~socket_runtime() {
        if (ready()) {
            WSACleanup();
        }
    }

    [[nodiscard]] bool ready() const { return result_ == 0; }

private:
    WSADATA data_{};
    int result_ = 0;
};
#else
using socket_handle = int;

constexpr socket_handle invalid_socket_handle = -1;

void close_socket(socket_handle fd) {
    ::close(fd);
}

class socket_runtime {
public:
    [[nodiscard]] bool ready() const { return true; }
};
#endif

class test_listener {
public:
    test_listener() = default;
    test_listener(const test_listener&) = delete;
    test_listener& operator=(const test_listener&) = delete;
    test_listener(test_listener&& other) noexcept
        : fd_(std::exchange(other.fd_, invalid_socket_handle)), port_(other.port_) {}
    test_listener& operator=(test_listener&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = std::exchange(other.fd_, invalid_socket_handle);
            port_ = other.port_;
        }
        return *this;
    }
    ~test_listener() { close(); }

    [[nodiscard]] std::uint16_t port() const { return port_; }

    static std::optional<test_listener> bind_ephemeral() {
        test_listener listener;
        listener.fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener.fd_ == invalid_socket_handle) {
            return std::nullopt;
        }

#ifdef _WIN32
        BOOL exclusive = TRUE;
        (void)::setsockopt(listener.fd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                           reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#endif

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(0);

        if (::bind(listener.fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return std::nullopt;
        }
        if (::listen(listener.fd_, 1) != 0) {
            return std::nullopt;
        }

        sockaddr_in bound_address{};
#ifdef _WIN32
        int length = sizeof(bound_address);
#else
        socklen_t length = sizeof(bound_address);
#endif
        if (::getsockname(listener.fd_, reinterpret_cast<sockaddr*>(&bound_address), &length) != 0) {
            return std::nullopt;
        }
        listener.port_ = ntohs(bound_address.sin_port);
        return listener;
    }

private:
    void close() {
        if (fd_ != invalid_socket_handle) {
            close_socket(fd_);
            fd_ = invalid_socket_handle;
        }
    }

    socket_handle fd_ = invalid_socket_handle;
    std::uint16_t port_ = 0;
};

}  // namespace

int main() {
    bool ok = true;

    socket_runtime runtime;
    ok &= expect(runtime.ready(), "socket runtime did not start");

    auto ephemeral = mirage::probe_tcp_port_available(0);
    ok &= expect(ephemeral.available, "ephemeral tcp port should be available");

    auto listener = test_listener::bind_ephemeral();
    ok &= expect(listener.has_value(), "could not bind test listener");
    if (listener) {
        const auto port = listener->port();
        auto occupied = mirage::probe_tcp_port_available(port);
        ok &= expect(!occupied.available, "occupied tcp port should not be available");

        listener.reset();
        auto released = mirage::probe_tcp_port_available(port);
        ok &= expect(released.available, "released tcp port should be available");
    }

    return ok ? 0 : 1;
}
