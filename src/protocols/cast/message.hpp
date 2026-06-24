#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "core/receiver_client.hpp"

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

enum class channel_event : uint8_t {
    none,
    default_media_started,
    default_media_stopped,
    volume_updated,
    media_loaded,
    media_playback_updated,
    media_stopped,
    receiver_command_rejected,
    media_command_rejected,
};

struct channel_activity {
    channel_event event = channel_event::none;
    std::string detail;
};

struct channel_message_result {
    std::vector<channel_message> responses;
    channel_activity activity;
    std::optional<receiver_client_media_status> media_status;
};

struct channel_session_state {
    bool default_media_running = false;
    double volume_level = 1.0;
    bool volume_muted = false;
    bool media_session_active = false;
    int64_t media_session_id = 1;
    double media_current_time = 0.0;
    double media_duration = 0.0;
    double media_playback_rate = 1.0;
    std::string media_player_state = "IDLE";
    std::string media_content_id;
    std::string media_content_type;
    std::string media_title;
    std::string media_artist;
    std::string media_album;
    uint64_t accepted_media_loads = 0;
    uint64_t rejected_media_commands = 0;
    std::string last_media_error;
};

result<channel_message> parse_channel_message(std::span<const std::byte> payload);
result<std::vector<std::byte>> serialize_channel_message(const channel_message& message);

channel_message_result handle_channel_message_result(const channel_message& message,
                                                     std::string_view device_name,
                                                     channel_session_state& state);
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
