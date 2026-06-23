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

    std::vector<std::byte> invalid_varint(12, std::byte{0x80});
    ok &= expect(!parse_channel_message(invalid_varint).has_value(),
                 "invalid protobuf varint parsed");

    return ok ? 0 : 1;
}
