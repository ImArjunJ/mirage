#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <variant>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace mirage::io {

class ip_address {
public:
    using v4_bytes = std::array<uint8_t, 4>;
    using v6_bytes = std::array<uint8_t, 16>;

    static constexpr ip_address v4(uint32_t host_order) {
        ip_address a;
        a.data_ = v4_data{host_order};
        return a;
    }

    static constexpr ip_address v4(v4_bytes bytes) {
        uint32_t val = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                       (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
        return v4(val);
    }

    static constexpr ip_address v6(v6_bytes bytes, uint32_t scope = 0) {
        ip_address a;
        a.data_ = v6_data{bytes, scope};
        return a;
    }

    static constexpr ip_address v4_any() { return v4(0); }
    static constexpr ip_address v4_loopback() { return v4(0x7f000001); }

    static ip_address parse(std::string_view str) {
        std::string s(str);
        in_addr addr4{};
        if (inet_pton(AF_INET, s.c_str(), &addr4) == 1) {
            return v4(ntohl(addr4.s_addr));
        }
        in6_addr addr6{};
        if (inet_pton(AF_INET6, s.c_str(), &addr6) == 1) {
            v6_bytes bytes;
            std::memcpy(bytes.data(), &addr6, 16);
            return v6(bytes);
        }
        return v4_any();
    }

    [[nodiscard]] constexpr bool is_v4() const { return std::holds_alternative<v4_data>(data_); }
    [[nodiscard]] constexpr bool is_v6() const { return std::holds_alternative<v6_data>(data_); }

    [[nodiscard]] constexpr uint32_t to_v4_uint() const { return std::get<v4_data>(data_).value; }

    [[nodiscard]] constexpr v4_bytes to_v4_bytes() const {
        auto val = to_v4_uint();
        return {uint8_t(val >> 24), uint8_t(val >> 16), uint8_t(val >> 8), uint8_t(val)};
    }

    [[nodiscard]] constexpr v6_bytes to_v6_bytes() const { return std::get<v6_data>(data_).bytes; }
    [[nodiscard]] constexpr uint32_t scope_id() const { return std::get<v6_data>(data_).scope; }

    [[nodiscard]] std::string to_string() const {
        if (is_v4()) {
            auto b = to_v4_bytes();
            return std::format("{}.{}.{}.{}", b[0], b[1], b[2], b[3]);
        }
        auto& d = std::get<v6_data>(data_);
        char buf[INET6_ADDRSTRLEN]{};
        in6_addr addr6{};
        std::memcpy(&addr6, d.bytes.data(), 16);
        inet_ntop(AF_INET6, &addr6, buf, sizeof(buf));
        std::string result(buf);
        if (d.scope != 0) {
            result += std::format("%{}", d.scope);
        }
        return result;
    }

    constexpr bool operator==(const ip_address&) const = default;
    constexpr auto operator<=>(const ip_address&) const = default;

private:
    struct v4_data {
        uint32_t value = 0;
        constexpr bool operator==(const v4_data&) const = default;
        constexpr auto operator<=>(const v4_data&) const = default;
    };

    struct v6_data {
        v6_bytes bytes{};
        uint32_t scope = 0;
        constexpr bool operator==(const v6_data&) const = default;
        constexpr auto operator<=>(const v6_data&) const = default;
    };

    std::variant<v4_data, v6_data> data_{v4_data{}};
};

struct endpoint {
    ip_address addr;
    uint16_t port = 0;

    [[nodiscard]] std::string to_string() const {
        if (addr.is_v6()) {
            return std::format("[{}]:{}", addr.to_string(), port);
        }
        return std::format("{}:{}", addr.to_string(), port);
    }

    bool operator==(const endpoint&) const = default;
};

inline std::string host_name() {
    char buf[256]{};
    ::gethostname(buf, sizeof(buf));
    return std::string(buf);
}

}  // namespace mirage::io
