#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protocols/cast/message.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

}  // namespace

int main() {
    using namespace mirage::protocols::cast;
    bool ok = true;

    channel_message status_request{
        .protocol_version = 0,
        .source_id = "sender-0",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"GET_STATUS\",\"requestId\":9}",
        .payload_binary = {},
    };

    auto encoded = serialize_channel_message(status_request);
    ok &= expect(encoded.has_value(), "status request serialization failed");
    if (encoded) {
        auto parsed = parse_channel_message(*encoded);
        ok &= expect(parsed.has_value(), "status request parse failed");
        if (!parsed) {
            return 1;
        }
        ok &= expect(parsed->source_id == "sender-0", "source id mismatch");
        ok &= expect(parsed->destination_id == "receiver-0", "destination id mismatch");
        ok &= expect(parsed->namespace_ == namespace_receiver, "namespace mismatch");
        ok &= expect(parsed->payload_utf8 == "{\"type\":\"GET_STATUS\",\"requestId\":9}",
                     "payload mismatch");

        auto responses = handle_channel_message(*parsed, "Living Room");
        ok &= expect(responses.size() == 1, "status response count mismatch");
        if (!responses.empty()) {
            const auto& response = responses.front();
            ok &= expect(response.source_id == receiver_source_id, "status response source");
            ok &= expect(response.destination_id == "sender-0", "status response destination");
            ok &= expect(response.namespace_ == namespace_receiver, "status response namespace");
            ok &= expect(contains(response.payload_utf8, "\"type\":\"RECEIVER_STATUS\""),
                         "status response type mismatch");
            ok &= expect(contains(response.payload_utf8, "\"requestId\":9"),
                         "status response request id mismatch");
            ok &= expect(contains(response.payload_utf8, "\"friendlyName\":\"Living Room\""),
                         "status response friendly name mismatch");
        }
    }

    channel_message ping{
        .protocol_version = 0,
        .source_id = "sender-1",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_heartbeat),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"PING\"}",
        .payload_binary = {},
    };
    auto pong = handle_channel_message(ping, "Living Room");
    ok &= expect(pong.size() == 1, "pong response count mismatch");
    if (!pong.empty()) {
        ok &= expect(pong.front().namespace_ == namespace_heartbeat, "pong namespace mismatch");
        ok &= expect(pong.front().payload_utf8 == "{\"type\":\"PONG\"}",
                     "pong payload mismatch");
    }

    channel_message connect{
        .protocol_version = 0,
        .source_id = "sender-2",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_connection),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"CONNECT\"}",
        .payload_binary = {},
    };
    ok &= expect(handle_channel_message(connect, "Living Room").empty(),
                 "connect should not require a response");

    channel_message availability{
        .protocol_version = 0,
        .source_id = "sender-3",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 =
            "{\"type\":\"GET_APP_AVAILABILITY\",\"requestId\":10,"
            "\"appId\":[\"CC1AD845\",\"YouTube\"]}",
        .payload_binary = {},
    };
    channel_session_state state;

    auto availability_response = handle_channel_message(availability, "Living Room", state);
    ok &= expect(availability_response.size() == 1, "availability response count mismatch");
    if (!availability_response.empty()) {
        ok &= expect(contains(availability_response.front().payload_utf8,
                              "\"type\":\"GET_APP_AVAILABILITY\""),
                     "availability response type mismatch");
        ok &= expect(contains(availability_response.front().payload_utf8,
                              "\"CC1AD845\":\"APP_AVAILABLE\""),
                     "default media availability mismatch");
        ok &= expect(contains(availability_response.front().payload_utf8,
                              "\"YouTube\":\"APP_NOT_AVAILABLE\""),
                     "youtube availability mismatch");
        ok &= expect(contains(availability_response.front().payload_utf8, "\"requestId\":10"),
                     "availability request id mismatch");
    }

    channel_message launch{
        .protocol_version = 0,
        .source_id = "sender-4",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"LAUNCH\",\"requestId\":11,\"appId\":\"CC1AD845\"}",
        .payload_binary = {},
    };
    auto launch_result = handle_channel_message_result(launch, "Living Room", state);
    auto& launch_response = launch_result.responses;
    ok &= expect(launch_response.size() == 1, "launch response count mismatch");
    if (!launch_response.empty()) {
        ok &= expect(contains(launch_response.front().payload_utf8,
                              "\"type\":\"RECEIVER_STATUS\""),
                     "launch status type mismatch");
        ok &= expect(contains(launch_response.front().payload_utf8,
                              "\"appId\":\"CC1AD845\""),
                     "launch app id mismatch");
        ok &= expect(contains(launch_response.front().payload_utf8,
                              "\"transportId\":\"web-1\""),
                     "launch transport id mismatch");
        ok &= expect(contains(launch_response.front().payload_utf8, "\"requestId\":11"),
                     "launch request id mismatch");
        ok &= expect(launch_result.activity.event == channel_event::default_media_started,
                     "launch activity mismatch");
        ok &= expect(state.default_media_running, "launch state mismatch");
    }

    channel_message unknown_launch{
        .protocol_version = 0,
        .source_id = "sender-4",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"LAUNCH\",\"requestId\":17,\"appId\":\"YouTube\"}",
        .payload_binary = {},
    };
    auto unknown_launch_response = handle_channel_message(unknown_launch, "Living Room", state);
    ok &= expect(unknown_launch_response.size() == 1, "unknown launch response count mismatch");
    if (!unknown_launch_response.empty()) {
        ok &= expect(contains(unknown_launch_response.front().payload_utf8,
                              "\"type\":\"LAUNCH_ERROR\""),
                     "unknown launch error type mismatch");
        ok &= expect(contains(unknown_launch_response.front().payload_utf8,
                              "\"reason\":\"NOT_SUPPORTED\""),
                     "unknown launch error reason mismatch");
        ok &= expect(contains(unknown_launch_response.front().payload_utf8, "\"requestId\":17"),
                     "unknown launch request id mismatch");
    }

    channel_message set_volume{
        .protocol_version = 0,
        .source_id = "sender-5",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 =
            "{\"type\":\"SET_VOLUME\",\"requestId\":12,"
            "\"volume\":{\"level\":0.42,\"muted\":true}}",
        .payload_binary = {},
    };
    auto volume_result = handle_channel_message_result(set_volume, "Living Room", state);
    auto& volume_response = volume_result.responses;
    ok &= expect(volume_response.size() == 1, "set volume response count mismatch");
    if (!volume_response.empty()) {
        ok &= expect(contains(volume_response.front().payload_utf8,
                              "\"type\":\"RECEIVER_STATUS\""),
                     "set volume status type mismatch");
        ok &= expect(contains(volume_response.front().payload_utf8, "\"requestId\":12"),
                     "set volume request id mismatch");
        ok &= expect(contains(volume_response.front().payload_utf8, "\"appId\":\"CC1AD845\""),
                     "set volume app state mismatch");
        ok &= expect(contains(volume_response.front().payload_utf8, "\"level\":0.42"),
                     "set volume level mismatch");
        ok &= expect(contains(volume_response.front().payload_utf8, "\"muted\":true"),
                     "set volume muted mismatch");
        ok &= expect(volume_result.activity.event == channel_event::volume_updated,
                     "set volume activity mismatch");
        ok &= expect(volume_result.activity.detail == "muted", "set volume activity detail");
        ok &= expect(state.volume_muted, "set volume muted state mismatch");
    }

    channel_message load{
        .protocol_version = 0,
        .source_id = "sender-6",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_media),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"LOAD\",\"requestId\":13}",
        .payload_binary = {},
    };
    auto load_result = handle_channel_message_result(load, "Living Room", state);
    auto& load_response = load_result.responses;
    ok &= expect(load_response.size() == 1, "load response count mismatch");
    if (!load_response.empty()) {
        ok &= expect(load_response.front().namespace_ == namespace_media,
                     "load response namespace mismatch");
        ok &= expect(contains(load_response.front().payload_utf8, "\"type\":\"LOAD_FAILED\""),
                     "load failure type mismatch");
        ok &= expect(contains(load_response.front().payload_utf8,
                              "\"reason\":\"MEDIA_NOT_SUPPORTED\""),
                     "load failure reason mismatch");
        ok &= expect(contains(load_response.front().payload_utf8, "\"requestId\":13"),
                     "load request id mismatch");
        ok &= expect(load_result.activity.event == channel_event::media_load_rejected,
                     "load activity mismatch");
        ok &= expect(load_result.activity.detail == "MEDIA_NOT_SUPPORTED",
                     "load activity detail mismatch");
        ok &= expect(state.rejected_media_loads == 1, "load rejection count mismatch");
    }

    channel_message media_status{
        .protocol_version = 0,
        .source_id = "sender-7",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_media),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"GET_STATUS\",\"requestId\":14}",
        .payload_binary = {},
    };
    auto media_status_response = handle_channel_message(media_status, "Living Room");
    ok &= expect(media_status_response.size() == 1, "media status response count mismatch");
    if (!media_status_response.empty()) {
        ok &= expect(media_status_response.front().namespace_ == namespace_media,
                     "media status response namespace mismatch");
        ok &= expect(contains(media_status_response.front().payload_utf8,
                              "\"type\":\"MEDIA_STATUS\""),
                     "media status response type mismatch");
        ok &= expect(contains(media_status_response.front().payload_utf8, "\"status\":[]"),
                     "media status response body mismatch");
        ok &= expect(contains(media_status_response.front().payload_utf8, "\"requestId\":14"),
                     "media status request id mismatch");
    }

    channel_message media_stop{
        .protocol_version = 0,
        .source_id = "sender-8",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_media),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"STOP\",\"requestId\":15,\"mediaSessionId\":1}",
        .payload_binary = {},
    };
    auto media_stop_response = handle_channel_message(media_stop, "Living Room");
    ok &= expect(media_stop_response.size() == 1, "media stop response count mismatch");
    if (!media_stop_response.empty()) {
        ok &= expect(contains(media_stop_response.front().payload_utf8,
                              "\"type\":\"MEDIA_STATUS\""),
                     "media stop status type mismatch");
        ok &= expect(contains(media_stop_response.front().payload_utf8, "\"status\":[]"),
                     "media stop status body mismatch");
        ok &= expect(contains(media_stop_response.front().payload_utf8, "\"requestId\":15"),
                     "media stop request id mismatch");
    }

    channel_message media_play{
        .protocol_version = 0,
        .source_id = "sender-9",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_media),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = "{\"type\":\"PLAY\",\"requestId\":16,\"mediaSessionId\":1}",
        .payload_binary = {},
    };
    auto media_play_result = handle_channel_message_result(media_play, "Living Room", state);
    auto& media_play_response = media_play_result.responses;
    ok &= expect(media_play_response.size() == 1, "media play response count mismatch");
    if (!media_play_response.empty()) {
        ok &= expect(contains(media_play_response.front().payload_utf8,
                              "\"type\":\"INVALID_REQUEST\""),
                     "media play invalid request type mismatch");
        ok &= expect(contains(media_play_response.front().payload_utf8,
                              "\"reason\":\"INVALID_MEDIA_SESSION_ID\""),
                     "media play invalid reason mismatch");
        ok &= expect(contains(media_play_response.front().payload_utf8, "\"requestId\":16"),
                     "media play request id mismatch");
        ok &= expect(media_play_result.activity.event == channel_event::media_command_rejected,
                     "media play activity mismatch");
        ok &= expect(state.rejected_media_commands == 1,
                     "media command rejection count mismatch");
    }

    channel_message stop{
        .protocol_version = 0,
        .source_id = "sender-10",
        .destination_id = "receiver-0",
        .namespace_ = std::string(namespace_receiver),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 =
            "{\"type\":\"STOP\",\"requestId\":18,\"sessionId\":\"default-media-session\"}",
        .payload_binary = {},
    };
    auto stop_result = handle_channel_message_result(stop, "Living Room", state);
    auto& stop_response = stop_result.responses;
    ok &= expect(stop_response.size() == 1, "receiver stop response count mismatch");
    if (!stop_response.empty()) {
        ok &= expect(contains(stop_response.front().payload_utf8,
                              "\"type\":\"RECEIVER_STATUS\""),
                     "receiver stop status type mismatch");
        ok &= expect(contains(stop_response.front().payload_utf8, "\"applications\":[]"),
                     "receiver stop app state mismatch");
        ok &= expect(contains(stop_response.front().payload_utf8, "\"requestId\":18"),
                     "receiver stop request id mismatch");
        ok &= expect(stop_result.activity.event == channel_event::default_media_stopped,
                     "receiver stop activity mismatch");
        ok &= expect(!state.default_media_running, "receiver stop state mismatch");
    }

    std::vector<std::byte> invalid_varint(12, std::byte{0x80});
    ok &= expect(!parse_channel_message(invalid_varint).has_value(),
                 "invalid protobuf varint parsed");

    return ok ? 0 : 1;
}
