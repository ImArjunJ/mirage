#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mirage::protocols::cast {

enum class probe_kind : uint8_t {
    http_options,
    http_status,
    tls_client_hello,
    channel_frame,
    unsupported,
};

struct probe_response {
    probe_kind kind = probe_kind::unsupported;
    std::string response;
};

probe_response handle_probe(std::string_view data, std::string_view device_name);

}  // namespace mirage::protocols::cast
