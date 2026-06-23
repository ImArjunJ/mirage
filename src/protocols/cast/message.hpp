#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/core.hpp"

namespace mirage::protocols::cast {

inline constexpr std::string_view namespace_connection =
    "urn:x-cast:com.google.cast.tp.connection";
inline constexpr std::string_view namespace_heartbeat = "urn:x-cast:com.google.cast.tp.heartbeat";
inline constexpr std::string_view namespace_receiver = "urn:x-cast:com.google.cast.receiver";
inline constexpr std::string_view receiver_source_id = "receiver-0";

enum class channel_payload_type : uint8_t {
    string_payload = 0,
    binary_payload = 1,
};

struct channel_message {
    uint32_t protocol_version = 0;
    std::string source_id;
    std::string destination_id;
    std::string namespace_;
    channel_payload_type payload_type = channel_payload_type::string_payload;
    std::string payload_utf8;
    std::vector<std::byte> payload_binary;
};

result<channel_message> parse_channel_message(std::span<const std::byte> payload);
result<std::vector<std::byte>> serialize_channel_message(const channel_message& message);

std::vector<channel_message> handle_channel_message(const channel_message& message,
                                                    std::string_view device_name);
std::string receiver_status_payload(std::string_view device_name,
                                    std::optional<int64_t> request_id);

}  // namespace mirage::protocols::cast
