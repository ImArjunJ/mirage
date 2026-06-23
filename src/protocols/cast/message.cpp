#include "protocols/cast/message.hpp"

#include <charconv>
#include <cctype>
#include <format>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace mirage::protocols::cast {
namespace {

enum class wire_type : uint8_t {
    varint = 0,
    length_delimited = 2,
};

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

result<uint64_t> read_varint(std::span<const std::byte> payload, size_t& offset) {
    uint64_t value = 0;
    uint32_t shift = 0;
    while (offset < payload.size() && shift < 64) {
        const auto byte = std::to_integer<uint8_t>(payload[offset++]);
        value |= static_cast<uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            return value;
        }
        shift += 7;
    }
    return std::unexpected(mirage_error::cast_err("invalid channel message varint"));
}

void write_varint(uint64_t value, std::vector<std::byte>& out) {
    while (value >= 0x80U) {
        out.push_back(std::byte{static_cast<uint8_t>((value & 0x7fU) | 0x80U)});
        value >>= 7;
    }
    out.push_back(std::byte{static_cast<uint8_t>(value)});
}

void write_key(uint32_t field_number, wire_type type, std::vector<std::byte>& out) {
    write_varint((static_cast<uint64_t>(field_number) << 3) | static_cast<uint8_t>(type), out);
}

result<std::span<const std::byte>> read_length_delimited(std::span<const std::byte> payload,
                                                         size_t& offset) {
    auto length = read_varint(payload, offset);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (*length > payload.size() - offset) {
        return std::unexpected(mirage_error::cast_err("truncated channel message field"));
    }
    auto field = payload.subspan(offset, static_cast<size_t>(*length));
    offset += static_cast<size_t>(*length);
    return field;
}

void write_length_delimited(uint32_t field_number, std::span<const std::byte> value,
                            std::vector<std::byte>& out) {
    write_key(field_number, wire_type::length_delimited, out);
    write_varint(value.size(), out);
    out.insert(out.end(), value.begin(), value.end());
}

void write_string(uint32_t field_number, std::string_view value, std::vector<std::byte>& out) {
    write_length_delimited(field_number, std::as_bytes(std::span<const char>(value.data(),
                                                                             value.size())),
                           out);
}

result<void> skip_field(std::span<const std::byte> payload, wire_type type, size_t& offset) {
    switch (type) {
        case wire_type::varint: {
            auto ignored = read_varint(payload, offset);
            if (!ignored) {
                return std::unexpected(ignored.error());
            }
            return {};
        }
        case wire_type::length_delimited: {
            auto ignored = read_length_delimited(payload, offset);
            if (!ignored) {
                return std::unexpected(ignored.error());
            }
            return {};
        }
    }
    return std::unexpected(mirage_error::cast_err("unsupported channel message wire type"));
}

std::string bytes_to_string(std::span<const std::byte> value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::optional<std::string> extract_json_string(std::string_view json, std::string_view key) {
    const auto needle = std::format("\"{}\"", key);
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return std::nullopt;
    }
    ++pos;

    std::string value;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            value.push_back(json[pos++]);
            continue;
        }
        if (c == '"') {
            return value;
        }
        value.push_back(c);
    }
    return std::nullopt;
}

std::vector<std::string> extract_json_string_array(std::string_view json, std::string_view key) {
    std::vector<std::string> values;
    const auto needle = std::format("\"{}\"", key);
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return values;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return values;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }
    if (pos >= json.size()) {
        return values;
    }
    if (json[pos] == '"') {
        if (auto single = extract_json_string(json, key)) {
            values.push_back(std::move(*single));
        }
        return values;
    }
    if (json[pos] != '[') {
        return values;
    }
    ++pos;

    while (pos < json.size()) {
        while (pos < json.size() &&
               (std::isspace(static_cast<unsigned char>(json[pos])) != 0 || json[pos] == ',')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] == ']') {
            break;
        }
        if (json[pos] != '"') {
            break;
        }
        ++pos;

        std::string value;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '\\' && pos < json.size()) {
                value.push_back(json[pos++]);
                continue;
            }
            if (c == '"') {
                values.push_back(std::move(value));
                break;
            }
            value.push_back(c);
        }
    }

    return values;
}

std::optional<int64_t> extract_json_int(std::string_view json, std::string_view key) {
    const auto needle = std::format("\"{}\"", key);
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos])) != 0) {
        ++pos;
    }
    auto end = pos;
    if (end < json.size() && json[end] == '-') {
        ++end;
    }
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])) != 0) {
        ++end;
    }
    if (end == pos) {
        return std::nullopt;
    }

    int64_t value = 0;
    auto [ptr, ec] = std::from_chars(json.data() + pos, json.data() + end, value);
    if (ec != std::errc{} || ptr != json.data() + end) {
        return std::nullopt;
    }
    return value;
}

std::string request_id_fragment(std::optional<int64_t> request_id) {
    if (!request_id) {
        return {};
    }
    return std::format(",\"requestId\":{}", *request_id);
}

channel_message make_string_message(std::string_view destination_id, std::string_view namespace_,
                                    std::string payload) {
    return {
        .protocol_version = 0,
        .source_id = std::string(receiver_source_id),
        .destination_id = std::string(destination_id),
        .namespace_ = std::string(namespace_),
        .payload_type = channel_payload_type::string_payload,
        .payload_utf8 = std::move(payload),
        .payload_binary = {},
    };
}

bool is_default_media_app(std::string_view app_id) {
    return app_id == default_media_app_id;
}

std::string launch_error_payload(std::optional<int64_t> request_id, std::string_view reason) {
    return std::format("{{\"type\":\"LAUNCH_ERROR\",\"reason\":\"{}\"{}}}",
                       json_escape(reason), request_id_fragment(request_id));
}

std::string load_failed_payload(std::optional<int64_t> request_id, std::string_view reason) {
    return std::format("{{\"type\":\"LOAD_FAILED\",\"reason\":\"{}\"{}}}", json_escape(reason),
                       request_id_fragment(request_id));
}

std::string media_status_payload(std::optional<int64_t> request_id) {
    auto body = std::string("{\"type\":\"MEDIA_STATUS\",\"status\":[]");
    body += request_id_fragment(request_id);
    body += "}";
    return body;
}

std::string media_invalid_request_payload(std::optional<int64_t> request_id,
                                          std::string_view reason) {
    return std::format("{{\"type\":\"INVALID_REQUEST\",\"reason\":\"{}\"{}}}",
                       json_escape(reason), request_id_fragment(request_id));
}

std::string app_availability_payload(std::span<const std::string> app_ids,
                                     std::optional<int64_t> request_id) {
    std::string body = "{\"type\":\"GET_APP_AVAILABILITY\",\"availability\":{";
    for (size_t i = 0; i < app_ids.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body += std::format("\"{}\":\"{}\"", json_escape(app_ids[i]),
                            is_default_media_app(app_ids[i]) ? "APP_AVAILABLE"
                                                             : "APP_NOT_AVAILABLE");
    }
    body += "}";
    body += request_id_fragment(request_id);
    body += "}";
    return body;
}

}  // namespace

result<channel_message> parse_channel_message(std::span<const std::byte> payload) {
    channel_message message;
    size_t offset = 0;
    while (offset < payload.size()) {
        auto key = read_varint(payload, offset);
        if (!key) {
            return std::unexpected(key.error());
        }
        const auto field_number = static_cast<uint32_t>(*key >> 3);
        const auto type = static_cast<wire_type>(*key & 0x07U);

        switch (field_number) {
            case 1: {
                if (type != wire_type::varint) {
                    return std::unexpected(
                        mirage_error::cast_err("invalid protocol_version field type"));
                }
                auto value = read_varint(payload, offset);
                if (!value) {
                    return std::unexpected(value.error());
                }
                if (*value > std::numeric_limits<uint32_t>::max()) {
                    return std::unexpected(
                        mirage_error::cast_err("protocol_version field is too large"));
                }
                message.protocol_version = static_cast<uint32_t>(*value);
                break;
            }
            case 2:
            case 3:
            case 4:
            case 6: {
                if (type != wire_type::length_delimited) {
                    return std::unexpected(mirage_error::cast_err("invalid string field type"));
                }
                auto value = read_length_delimited(payload, offset);
                if (!value) {
                    return std::unexpected(value.error());
                }
                auto text = bytes_to_string(*value);
                if (field_number == 2) {
                    message.source_id = std::move(text);
                } else if (field_number == 3) {
                    message.destination_id = std::move(text);
                } else if (field_number == 4) {
                    message.namespace_ = std::move(text);
                } else {
                    message.payload_utf8 = std::move(text);
                }
                break;
            }
            case 5: {
                if (type != wire_type::varint) {
                    return std::unexpected(
                        mirage_error::cast_err("invalid payload_type field type"));
                }
                auto value = read_varint(payload, offset);
                if (!value) {
                    return std::unexpected(value.error());
                }
                if (*value == 0) {
                    message.payload_type = channel_payload_type::string_payload;
                } else if (*value == 1) {
                    message.payload_type = channel_payload_type::binary_payload;
                } else {
                    return std::unexpected(
                        mirage_error::cast_err("unsupported channel payload type"));
                }
                break;
            }
            case 7: {
                if (type != wire_type::length_delimited) {
                    return std::unexpected(mirage_error::cast_err("invalid binary field type"));
                }
                auto value = read_length_delimited(payload, offset);
                if (!value) {
                    return std::unexpected(value.error());
                }
                message.payload_binary.assign(value->begin(), value->end());
                break;
            }
            default: {
                auto skipped = skip_field(payload, type, offset);
                if (!skipped) {
                    return std::unexpected(skipped.error());
                }
                break;
            }
        }
    }

    return message;
}

result<std::vector<std::byte>> serialize_channel_message(const channel_message& message) {
    std::vector<std::byte> payload;
    payload.reserve(message.payload_utf8.size() + message.payload_binary.size() +
                    message.source_id.size() + message.destination_id.size() +
                    message.namespace_.size() + 24);

    write_key(1, wire_type::varint, payload);
    write_varint(message.protocol_version, payload);
    write_string(2, message.source_id, payload);
    write_string(3, message.destination_id, payload);
    write_string(4, message.namespace_, payload);
    write_key(5, wire_type::varint, payload);
    write_varint(static_cast<uint8_t>(message.payload_type), payload);
    if (message.payload_type == channel_payload_type::string_payload) {
        write_string(6, message.payload_utf8, payload);
    } else {
        write_length_delimited(7, message.payload_binary, payload);
    }

    return payload;
}

std::string receiver_status_payload(std::string_view device_name,
                                    std::optional<int64_t> request_id) {
    channel_session_state state;
    return receiver_status_payload(device_name, request_id, state);
}

std::string receiver_status_payload(std::string_view device_name,
                                    std::optional<int64_t> request_id,
                                    const channel_session_state& state) {
    std::string applications = "[]";
    if (state.default_media_running) {
        applications =
            std::format("[{{\"appId\":\"{}\","
                        "\"displayName\":\"Default Media Receiver\","
                        "\"namespaces\":[{{\"name\":\"{}\"}}],"
                        "\"sessionId\":\"default-media-session\","
                        "\"statusText\":\"ready\","
                        "\"transportId\":\"{}\"}}]",
                        default_media_app_id, namespace_media, default_media_transport_id);
    }
    auto body = std::format(
        "{{\"type\":\"RECEIVER_STATUS\","
        "\"status\":{{\"applications\":{},"
        "\"volume\":{{\"controlType\":\"attenuation\",\"level\":1.0,\"muted\":false}},"
        "\"isActiveInput\":true,"
        "\"friendlyName\":\"{}\"}}",
        applications, json_escape(device_name));
    body += request_id_fragment(request_id);
    body += "}";
    return body;
}

std::vector<channel_message> handle_channel_message(const channel_message& message,
                                                    std::string_view device_name) {
    channel_session_state state;
    return handle_channel_message(message, device_name, state);
}

std::vector<channel_message> handle_channel_message(const channel_message& message,
                                                    std::string_view device_name,
                                                    channel_session_state& state) {
    std::vector<channel_message> responses;
    if (message.payload_type != channel_payload_type::string_payload) {
        return responses;
    }

    auto type = extract_json_string(message.payload_utf8, "type");
    if (!type) {
        return responses;
    }

    if (message.namespace_ == namespace_heartbeat && *type == "PING") {
        responses.push_back(make_string_message(message.source_id, namespace_heartbeat,
                                                "{\"type\":\"PONG\"}"));
        return responses;
    }

    if (message.namespace_ == namespace_receiver && *type == "GET_STATUS") {
        responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            receiver_status_payload(device_name, extract_json_int(message.payload_utf8,
                                                                  "requestId"),
                                    state)));
        return responses;
    }

    if (message.namespace_ == namespace_receiver && *type == "GET_APP_AVAILABILITY") {
        auto app_ids = extract_json_string_array(message.payload_utf8, "appId");
        responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            app_availability_payload(app_ids, extract_json_int(message.payload_utf8,
                                                               "requestId"))));
        return responses;
    }

    if (message.namespace_ == namespace_receiver && *type == "LAUNCH") {
        auto app_id = extract_json_string(message.payload_utf8, "appId").value_or("");
        if (is_default_media_app(app_id)) {
            state.default_media_running = true;
            responses.push_back(make_string_message(
                message.source_id, namespace_receiver,
                receiver_status_payload(device_name,
                                        extract_json_int(message.payload_utf8, "requestId"),
                                        state)));
            return responses;
        }
        responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            launch_error_payload(extract_json_int(message.payload_utf8, "requestId"),
                                 "NOT_SUPPORTED")));
        return responses;
    }

    if (message.namespace_ == namespace_receiver && *type == "STOP") {
        state.default_media_running = false;
        responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            receiver_status_payload(device_name, extract_json_int(message.payload_utf8,
                                                                  "requestId"),
                                    state)));
        return responses;
    }

    if (message.namespace_ == namespace_receiver && *type == "SET_VOLUME") {
        responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            receiver_status_payload(device_name, extract_json_int(message.payload_utf8,
                                                                  "requestId"),
                                    state)));
        return responses;
    }

    if (message.namespace_ == namespace_media && *type == "LOAD") {
        const auto reason = state.default_media_running ? "MEDIA_NOT_SUPPORTED"
                                                       : "RECEIVER_APP_NOT_RUNNING";
        responses.push_back(make_string_message(
            message.source_id, namespace_media,
            load_failed_payload(extract_json_int(message.payload_utf8, "requestId"), reason)));
        return responses;
    }

    if (message.namespace_ == namespace_media && *type == "GET_STATUS") {
        responses.push_back(make_string_message(
            message.source_id, namespace_media,
            media_status_payload(extract_json_int(message.payload_utf8, "requestId"))));
        return responses;
    }

    if (message.namespace_ == namespace_media && *type == "STOP") {
        responses.push_back(make_string_message(
            message.source_id, namespace_media,
            media_status_payload(extract_json_int(message.payload_utf8, "requestId"))));
        return responses;
    }

    if (message.namespace_ == namespace_media &&
        (*type == "PLAY" || *type == "PAUSE" || *type == "SEEK" ||
         *type == "SET_PLAYBACK_RATE" || *type == "EDIT_TRACKS_INFO")) {
        responses.push_back(make_string_message(
            message.source_id, namespace_media,
            media_invalid_request_payload(extract_json_int(message.payload_utf8, "requestId"),
                                          "INVALID_MEDIA_SESSION_ID")));
        return responses;
    }

    return responses;
}

}  // namespace mirage::protocols::cast
