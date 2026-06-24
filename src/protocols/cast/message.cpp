#include "protocols/cast/message.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace mirage::protocols::cast {
namespace {

inline constexpr std::string_view default_media_session_id = "default-media-session";

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
    write_length_delimited(field_number,
                           std::as_bytes(std::span<const char>(value.data(), value.size())), out);
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

std::optional<double> extract_json_double(std::string_view json, std::string_view key) {
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
    double sign = 1.0;
    if (pos < json.size() && json[pos] == '-') {
        sign = -1.0;
        ++pos;
    }

    double integer = 0.0;
    bool has_digits = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
        integer = integer * 10.0 + static_cast<double>(json[pos] - '0');
        has_digits = true;
        ++pos;
    }

    double fraction = 0.0;
    double scale = 1.0;
    if (pos < json.size() && json[pos] == '.') {
        ++pos;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
            fraction = fraction * 10.0 + static_cast<double>(json[pos] - '0');
            scale *= 10.0;
            has_digits = true;
            ++pos;
        }
    }

    if (!has_digits) {
        return std::nullopt;
    }

    double value = sign * (integer + fraction / scale);
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        ++pos;
        int exponent_sign = 1;
        if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) {
            exponent_sign = json[pos] == '-' ? -1 : 1;
            ++pos;
        }

        int exponent = 0;
        bool has_exponent_digits = false;
        while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos])) != 0) {
            exponent = exponent * 10 + (json[pos] - '0');
            has_exponent_digits = true;
            ++pos;
        }
        if (!has_exponent_digits) {
            return std::nullopt;
        }
        value *= std::pow(10.0, static_cast<double>(exponent_sign * exponent));
    }

    if (!std::isfinite(value)) {
        return std::nullopt;
    }

    return value;
}

std::optional<bool> extract_json_bool(std::string_view json, std::string_view key) {
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
    if (json.substr(pos, 4) == "true") {
        return true;
    }
    if (json.substr(pos, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::string json_number(double value) {
    auto text = std::format("{:.3f}", value);
    while (text.size() > 1 && text.back() == '0' && text[text.size() - 2] != '.') {
        text.pop_back();
    }
    return text;
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

bool default_media_session_matches(std::string_view payload) {
    auto session_id = extract_json_string(payload, "sessionId");
    return !session_id || *session_id == default_media_session_id;
}

std::string launch_error_payload(std::optional<int64_t> request_id, std::string_view reason) {
    return std::format("{{\"type\":\"LAUNCH_ERROR\",\"reason\":\"{}\"{}}}", json_escape(reason),
                       request_id_fragment(request_id));
}

float cast_volume_db(double level) {
    const auto clamped = std::clamp(level, 0.0, 1.0);
    if (clamped <= 0.0) {
        return -144.0F;
    }
    return static_cast<float>(20.0 * std::log10(clamped));
}

receiver_client_media_status receiver_media_status(const channel_session_state& state) {
    const auto position_ms = state.media_current_time <= 0.0
                                 ? 0ULL
                                 : static_cast<uint64_t>(state.media_current_time * 1000.0);
    const auto duration_ms =
        state.media_duration <= 0.0 ? 0ULL : static_cast<uint64_t>(state.media_duration * 1000.0);
    return {
        .active = state.media_session_active,
        .title = state.media_title,
        .artist = state.media_artist,
        .album = state.media_album,
        .artwork_type = {},
        .artwork_bytes = 0,
        .position_ms = position_ms,
        .duration_ms = duration_ms,
        .volume_db = cast_volume_db(state.volume_level),
        .volume_linear = static_cast<float>(std::clamp(state.volume_level, 0.0, 1.0)),
    };
}

bool add_connected_source(channel_session_state& state, std::string_view source_id) {
    if (source_id.empty()) {
        return false;
    }
    auto existing = std::find(state.connected_sources.begin(), state.connected_sources.end(),
                              source_id);
    if (existing != state.connected_sources.end()) {
        return false;
    }
    state.connected_sources.emplace_back(source_id);
    return true;
}

bool remove_connected_source(channel_session_state& state, std::string_view source_id) {
    auto existing = std::find(state.connected_sources.begin(), state.connected_sources.end(),
                              source_id);
    if (existing == state.connected_sources.end()) {
        return false;
    }
    state.connected_sources.erase(existing);
    return true;
}

void clear_media_session(channel_session_state& state) {
    state.media_session_active = false;
    state.media_current_time = 0.0;
    state.media_duration = 0.0;
    state.media_playback_rate = 1.0;
    state.media_player_state = "IDLE";
    state.media_content_id.clear();
    state.media_content_type.clear();
    state.media_title.clear();
    state.media_artist.clear();
    state.media_album.clear();
}

bool media_session_matches(std::string_view payload, const channel_session_state& state) {
    auto media_session_id = extract_json_int(payload, "mediaSessionId");
    return !media_session_id || *media_session_id == state.media_session_id;
}

std::string media_status_payload(std::optional<int64_t> request_id,
                                 const channel_session_state& state) {
    if (!state.media_session_active) {
        auto body = std::string("{\"type\":\"MEDIA_STATUS\",\"status\":[]");
        body += request_id_fragment(request_id);
        body += "}";
        return body;
    }

    auto body = std::string("{\"type\":\"MEDIA_STATUS\",\"status\":[{");
    body += std::format("\"mediaSessionId\":{}", state.media_session_id);
    body += std::format(",\"playbackRate\":{}", json_number(state.media_playback_rate));
    body += std::format(",\"playerState\":\"{}\"", json_escape(state.media_player_state));
    body += std::format(",\"currentTime\":{}", json_number(state.media_current_time));
    body += ",\"supportedMediaCommands\":15";
    body += std::format(",\"volume\":{{\"level\":{},\"muted\":{}}}",
                        json_number(state.volume_level), state.volume_muted ? "true" : "false");
    body += ",\"media\":{";
    body += std::format("\"contentId\":\"{}\"", json_escape(state.media_content_id));
    body += ",\"streamType\":\"BUFFERED\"";
    body += std::format(
        ",\"contentType\":\"{}\"",
        json_escape(state.media_content_type.empty() ? std::string_view("application/octet-stream")
                                                     : std::string_view(state.media_content_type)));
    if (state.media_duration > 0.0) {
        body += std::format(",\"duration\":{}", json_number(state.media_duration));
    }
    body += ",\"metadata\":{";
    body += "\"metadataType\":0";
    if (!state.media_title.empty()) {
        body += std::format(",\"title\":\"{}\"", json_escape(state.media_title));
    }
    if (!state.media_artist.empty()) {
        body += std::format(",\"artist\":\"{}\"", json_escape(state.media_artist));
    }
    if (!state.media_album.empty()) {
        body += std::format(",\"albumName\":\"{}\"", json_escape(state.media_album));
    }
    body += "}}";
    body += "}]";
    body += request_id_fragment(request_id);
    body += "}";
    return body;
}

std::string invalid_request_payload(std::optional<int64_t> request_id, std::string_view reason) {
    return std::format("{{\"type\":\"INVALID_REQUEST\",\"reason\":\"{}\"{}}}", json_escape(reason),
                       request_id_fragment(request_id));
}

void reject_receiver_command(channel_message_result& result, std::string_view destination_id,
                             std::optional<int64_t> request_id, std::string_view reason) {
    result.responses.push_back(make_string_message(destination_id, namespace_receiver,
                                                   invalid_request_payload(request_id, reason)));
    result.activity = {
        .event = channel_event::receiver_command_rejected,
        .detail = std::string(reason),
    };
}

void reject_media_command(channel_message_result& result, channel_session_state& state,
                          std::string_view destination_id, std::optional<int64_t> request_id,
                          std::string_view reason) {
    ++state.rejected_media_commands;
    state.last_media_error = reason;
    result.responses.push_back(make_string_message(destination_id, namespace_media,
                                                   invalid_request_payload(request_id, reason)));
    result.activity = {
        .event = channel_event::media_command_rejected,
        .detail = std::string(reason),
    };
}

std::string app_availability_payload(std::span<const std::string> app_ids,
                                     std::optional<int64_t> request_id) {
    std::string body = "{\"type\":\"GET_APP_AVAILABILITY\",\"availability\":{";
    for (size_t i = 0; i < app_ids.size(); ++i) {
        if (i != 0) {
            body += ',';
        }
        body +=
            std::format("\"{}\":\"{}\"", json_escape(app_ids[i]),
                        is_default_media_app(app_ids[i]) ? "APP_AVAILABLE" : "APP_NOT_AVAILABLE");
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

std::string receiver_status_payload(std::string_view device_name, std::optional<int64_t> request_id,
                                    const channel_session_state& state) {
    std::string applications = "[]";
    if (state.default_media_running) {
        applications = std::format(
            "[{{\"appId\":\"{}\","
            "\"displayName\":\"Default Media Receiver\","
            "\"namespaces\":[{{\"name\":\"{}\"}}],"
            "\"sessionId\":\"{}\","
            "\"statusText\":\"ready\","
            "\"transportId\":\"{}\"}}]",
            default_media_app_id, namespace_media, default_media_session_id,
            default_media_transport_id);
    }
    auto body = std::format(
        "{{\"type\":\"RECEIVER_STATUS\","
        "\"status\":{{\"applications\":{},"
        "\"volume\":{{\"controlType\":\"attenuation\",\"level\":{},\"muted\":{}}},"
        "\"isActiveInput\":true,"
        "\"friendlyName\":\"{}\"}}",
        applications, json_number(state.volume_level), state.volume_muted ? "true" : "false",
        json_escape(device_name));
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
    auto result = handle_channel_message_result(message, device_name, state);
    return std::move(result.responses);
}

channel_message_result handle_channel_message_result(const channel_message& message,
                                                     std::string_view device_name,
                                                     channel_session_state& state) {
    channel_message_result result;
    if (message.payload_type != channel_payload_type::string_payload) {
        return result;
    }

    auto type = extract_json_string(message.payload_utf8, "type");
    if (!type) {
        return result;
    }

    if (message.namespace_ == namespace_connection) {
        if (*type == "CONNECT") {
            if (add_connected_source(state, message.source_id)) {
                result.activity = {
                    .event = channel_event::channel_connected,
                    .detail = message.source_id,
                };
            }
            return result;
        }
        if (*type == "CLOSE") {
            if (remove_connected_source(state, message.source_id)) {
                result.activity = {
                    .event = channel_event::channel_closed,
                    .detail = message.source_id,
                };
            }
            return result;
        }
        return result;
    }

    if (message.namespace_ == namespace_heartbeat && *type == "PING") {
        result.responses.push_back(
            make_string_message(message.source_id, namespace_heartbeat, "{\"type\":\"PONG\"}"));
        return result;
    }

    if (message.namespace_ == namespace_receiver && *type == "GET_STATUS") {
        result.responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            receiver_status_payload(device_name,
                                    extract_json_int(message.payload_utf8, "requestId"), state)));
        return result;
    }

    if (message.namespace_ == namespace_receiver && *type == "GET_APP_AVAILABILITY") {
        auto app_ids = extract_json_string_array(message.payload_utf8, "appId");
        result.responses.push_back(
            make_string_message(message.source_id, namespace_receiver,
                                app_availability_payload(
                                    app_ids, extract_json_int(message.payload_utf8, "requestId"))));
        return result;
    }

    if (message.namespace_ == namespace_receiver && *type == "LAUNCH") {
        auto app_id = extract_json_string(message.payload_utf8, "appId").value_or("");
        if (is_default_media_app(app_id)) {
            state.default_media_running = true;
            result.responses.push_back(make_string_message(
                message.source_id, namespace_receiver,
                receiver_status_payload(
                    device_name, extract_json_int(message.payload_utf8, "requestId"), state)));
            result.activity = {
                .event = channel_event::default_media_started,
                .detail = std::string(default_media_app_id),
            };
            return result;
        }
        result.responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            launch_error_payload(extract_json_int(message.payload_utf8, "requestId"),
                                 "NOT_SUPPORTED")));
        return result;
    }

    if (message.namespace_ == namespace_receiver && *type == "STOP") {
        const auto request_id = extract_json_int(message.payload_utf8, "requestId");
        if (!default_media_session_matches(message.payload_utf8)) {
            result.responses.push_back(make_string_message(
                message.source_id, namespace_receiver,
                receiver_status_payload(device_name, request_id, state)));
            return result;
        }

        const bool had_default_media = state.default_media_running || state.media_session_active;
        state.default_media_running = false;
        if (state.media_session_active) {
            clear_media_session(state);
            result.media_status = receiver_media_status(state);
        }
        result.responses.push_back(make_string_message(
            message.source_id, namespace_receiver, receiver_status_payload(device_name, request_id,
                                                                           state)));
        if (had_default_media) {
            result.activity = {
                .event = channel_event::default_media_stopped,
                .detail = std::string(default_media_app_id),
            };
        }
        return result;
    }

    if (message.namespace_ == namespace_receiver && *type == "SET_VOLUME") {
        if (auto level = extract_json_double(message.payload_utf8, "level")) {
            state.volume_level = std::clamp(*level, 0.0, 1.0);
        }
        if (auto muted = extract_json_bool(message.payload_utf8, "muted")) {
            state.volume_muted = *muted;
        }
        if (state.media_session_active) {
            result.media_status = receiver_media_status(state);
        }
        result.responses.push_back(make_string_message(
            message.source_id, namespace_receiver,
            receiver_status_payload(device_name,
                                    extract_json_int(message.payload_utf8, "requestId"), state)));
        result.activity = {
            .event = channel_event::volume_updated,
            .detail = state.volume_muted ? "muted" : json_number(state.volume_level),
        };
        return result;
    }

    if (message.namespace_ == namespace_receiver) {
        reject_receiver_command(result, message.source_id,
                                extract_json_int(message.payload_utf8, "requestId"),
                                "INVALID_COMMAND");
        return result;
    }

    if (message.namespace_ == namespace_media && *type == "LOAD") {
        state.default_media_running = true;
        state.media_session_active = true;
        state.media_session_id = static_cast<int64_t>(++state.accepted_media_loads);
        state.media_content_id =
            extract_json_string(message.payload_utf8, "contentId").value_or("");
        state.media_content_type =
            extract_json_string(message.payload_utf8, "contentType").value_or("");
        state.media_title =
            extract_json_string(message.payload_utf8, "title").value_or(state.media_content_id);
        state.media_artist =
            extract_json_string(message.payload_utf8, "artist")
                .value_or(extract_json_string(message.payload_utf8, "subtitle").value_or(""));
        state.media_album =
            extract_json_string(message.payload_utf8, "albumName")
                .value_or(extract_json_string(message.payload_utf8, "album").value_or(""));
        state.media_duration =
            std::max(0.0, extract_json_double(message.payload_utf8, "duration").value_or(0.0));
        state.media_current_time =
            std::max(0.0, extract_json_double(message.payload_utf8, "currentTime").value_or(0.0));
        state.media_playback_rate = 1.0;
        const auto autoplay = extract_json_bool(message.payload_utf8, "autoplay").value_or(true);
        state.media_player_state = autoplay ? "PLAYING" : "PAUSED";
        state.last_media_error.clear();
        result.responses.push_back(make_string_message(
            message.source_id, namespace_media,
            media_status_payload(extract_json_int(message.payload_utf8, "requestId"), state)));
        result.activity = {
            .event = channel_event::media_loaded,
            .detail = state.media_title.empty() ? state.media_content_id : state.media_title,
        };
        result.media_status = receiver_media_status(state);
        result.media_load = channel_media_load{
            .url = state.media_content_id,
            .content_type = state.media_content_type,
            .title = state.media_title,
            .artist = state.media_artist,
            .album = state.media_album,
            .start_time = state.media_current_time,
            .duration = state.media_duration,
            .autoplay = autoplay,
        };
        return result;
    }

    if (message.namespace_ == namespace_media && *type == "GET_STATUS") {
        result.responses.push_back(make_string_message(
            message.source_id, namespace_media,
            media_status_payload(extract_json_int(message.payload_utf8, "requestId"), state)));
        return result;
    }

    if (message.namespace_ == namespace_media && *type == "STOP") {
        const auto request_id = extract_json_int(message.payload_utf8, "requestId");
        constexpr std::string_view reason = "INVALID_MEDIA_SESSION_ID";
        const auto requested_session_id = extract_json_int(message.payload_utf8, "mediaSessionId");
        if (!state.media_session_active) {
            if (requested_session_id) {
                reject_media_command(result, state, message.source_id, request_id, reason);
                return result;
            }
            result.responses.push_back(make_string_message(
                message.source_id, namespace_media, media_status_payload(request_id, state)));
            return result;
        }
        if (!media_session_matches(message.payload_utf8, state)) {
            reject_media_command(result, state, message.source_id, request_id, reason);
            return result;
        }
        clear_media_session(state);
        result.responses.push_back(make_string_message(
            message.source_id, namespace_media, media_status_payload(request_id, state)));
        result.activity = {
            .event = channel_event::media_stopped,
            .detail = "stopped",
        };
        result.media_status = receiver_media_status(state);
        return result;
    }

    if (message.namespace_ == namespace_media &&
        (*type == "PLAY" || *type == "PAUSE" || *type == "SEEK" || *type == "SET_PLAYBACK_RATE" ||
         *type == "EDIT_TRACKS_INFO")) {
        constexpr std::string_view reason = "INVALID_MEDIA_SESSION_ID";
        if (state.media_session_active && media_session_matches(message.payload_utf8, state)) {
            if (*type == "PLAY") {
                state.media_player_state = "PLAYING";
            } else if (*type == "PAUSE") {
                state.media_player_state = "PAUSED";
            } else if (*type == "SEEK") {
                if (auto current_time = extract_json_double(message.payload_utf8, "currentTime")) {
                    state.media_current_time = std::max(0.0, *current_time);
                }
            } else if (*type == "SET_PLAYBACK_RATE") {
                if (auto rate = extract_json_double(message.payload_utf8, "playbackRate")) {
                    state.media_playback_rate = std::max(0.0, *rate);
                }
            }
            result.responses.push_back(make_string_message(
                message.source_id, namespace_media,
                media_status_payload(extract_json_int(message.payload_utf8, "requestId"), state)));
            result.activity = {
                .event = channel_event::media_playback_updated,
                .detail = *type,
            };
            result.media_status = receiver_media_status(state);
            return result;
        }
        reject_media_command(result, state, message.source_id,
                             extract_json_int(message.payload_utf8, "requestId"), reason);
        return result;
    }

    if (message.namespace_ == namespace_media) {
        reject_media_command(result, state, message.source_id,
                             extract_json_int(message.payload_utf8, "requestId"),
                             "INVALID_COMMAND");
        return result;
    }

    return result;
}

}  // namespace mirage::protocols::cast
