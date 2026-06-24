#include "core/port_probe.hpp"

#include <format>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mirage {
namespace {

std::string socket_error_message(int code) {
    auto message = std::system_category().message(code);
    if (!message.empty()) {
        return message;
    }
    return std::format("socket error {}", code);
}

#ifdef _WIN32
class winsock_session {
public:
    winsock_session() : result_(WSAStartup(MAKEWORD(2, 2), &data_)) {}
    winsock_session(const winsock_session&) = delete;
    winsock_session& operator=(const winsock_session&) = delete;
    ~winsock_session() {
        if (ready()) {
            WSACleanup();
        }
    }

    [[nodiscard]] bool ready() const { return result_ == 0; }
    [[nodiscard]] int result() const { return result_; }

private:
    WSADATA data_{};
    int result_ = 0;
};
#endif

}  // namespace

tcp_port_probe_result probe_tcp_port_available(std::uint16_t port) {
#ifdef _WIN32
    winsock_session winsock;
    if (!winsock.ready()) {
        return {.available = false,
                .message =
                    std::format("winsock startup failed: {}", socket_error_message(winsock.result()))};
    }

    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) {
        const auto error = WSAGetLastError();
        return {.available = false, .message = socket_error_message(error)};
    }

    BOOL exclusive = TRUE;
    (void)::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        closesocket(fd);
        return {.available = false, .message = socket_error_message(error)};
    }

    closesocket(fd);
    return {.available = true, .message = {}};
#else
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {.available = false, .message = socket_error_message(errno)};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        const auto error = errno;
        ::close(fd);
        return {.available = false, .message = socket_error_message(error)};
    }

    ::close(fd);
    return {.available = true, .message = {}};
#endif
}

}  // namespace mirage
