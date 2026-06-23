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
inline constexpr std::string_view namespace_media = "urn:x-cast:com.google.cast.media";
inline constexpr std::string_view receiver_source_id = "receiver-0";
inline constexpr std::string_view default_media_app_id = "CC1AD845";
inline constexpr std::string_view default_media_transport_id = "web-1";

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

struct channel_session_state {
    bool default_media_running = false;
};

result<channel_message> parse_channel_message(std::span<const std::byte> payload);
result<std::vector<std::byte>> serialize_channel_message(const channel_message& message);

std::vector<channel_message> handle_channel_message(const channel_message& message,
                                                    std::string_view device_name);
std::vector<channel_message> handle_channel_message(const channel_message& message,
                                                    std::string_view device_name,
                                                    channel_session_state& state);
std::string receiver_status_payload(std::string_view device_name,
                                    std::optional<int64_t> request_id);
std::string receiver_status_payload(std::string_view device_name,
                                    std::optional<int64_t> request_id,
                                    const channel_session_state& state);

}  // namespace mirage::protocols::cast
