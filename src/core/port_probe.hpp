#pragma once

#include <cstdint>
#include <string>

namespace mirage {

struct tcp_port_probe_result {
    bool available = false;
    std::string message;
};

[[nodiscard]] tcp_port_probe_result probe_tcp_port_available(std::uint16_t port);

}  // namespace mirage
