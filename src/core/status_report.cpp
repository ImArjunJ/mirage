#include "core/status_report.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/core.hpp"

namespace mirage {
namespace {

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

std::string_view json_bool(bool value) {
    return value ? "true" : "false";
}

std::string json_unescape(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    bool escaped = false;
    for (char c : value) {
        if (escaped) {
            switch (c) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    out.push_back(c);
                    break;
            }
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) {
        out.push_back('\\');
    }
    return out;
}

std::optional<size_t> find_json_string_end(std::string_view value, size_t start) {
    bool escaped = false;
    for (size_t pos = start; pos < value.size(); ++pos) {
        const char c = value[pos];
        if (escaped) {
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return pos;
        }
    }
    return std::nullopt;
}

std::string extract_string_from(std::string_view object, std::string_view key) {
    const auto needle = std::format("\"{}\":\"", key);
    auto pos = object.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    auto end = find_json_string_end(object, pos);
    if (!end) {
        return {};
    }
    return json_unescape(object.substr(pos, *end - pos));
}

std::optional<int64_t> extract_int_from(std::string_view object, std::string_view key) {
    const auto needle = std::format("\"{}\":", key);
    auto pos = object.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    while (pos < object.size() && object[pos] == ' ') {
        ++pos;
    }
    try {
        return std::stoll(std::string(object.substr(pos)));
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> extract_bool_from(std::string_view object, std::string_view key) {
    const auto needle = std::format("\"{}\":", key);
    auto pos = object.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += needle.size();
    while (pos < object.size() && object[pos] == ' ') {
        ++pos;
    }
    if (object.substr(pos, 4) == "true") {
        return true;
    }
    if (object.substr(pos, 5) == "false") {
        return false;
    }
    return std::nullopt;
}

std::string_view extract_array_from(std::string_view object, std::string_view key) {
    const auto needle = std::format("\"{}\":[", key);
    auto pos = object.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    const auto array_start = pos + needle.size() - 1;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t scan = array_start; scan < object.size(); ++scan) {
        const char c = object[scan];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[') {
            ++depth;
        } else if (c == ']') {
            --depth;
            if (depth == 0) {
                return object.substr(array_start, scan - array_start + 1);
            }
        }
    }
    return {};
}

std::string_view extract_object_from(std::string_view object, std::string_view key) {
    const auto needle = std::format("\"{}\":{{", key);
    auto pos = object.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    const auto object_start = pos + needle.size() - 1;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t scan = object_start; scan < object.size(); ++scan) {
        const char c = object[scan];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return object.substr(object_start, scan - object_start + 1);
            }
        }
    }
    return {};
}

std::vector<std::string_view> extract_objects(std::string_view array) {
    std::vector<std::string_view> objects;
    size_t object_start = std::string_view::npos;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t pos = 0; pos < array.size(); ++pos) {
        const char c = array[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                object_start = pos;
            }
            ++depth;
        } else if (c == '}' && depth > 0) {
            --depth;
            if (depth == 0 && object_start != std::string_view::npos) {
                objects.push_back(array.substr(object_start, pos - object_start + 1));
                object_start = std::string_view::npos;
            }
        }
    }
    return objects;
}

std::string_view extract_protocol_object(std::string_view json, std::string_view id) {
    const auto needle = std::format("\"id\":\"{}\"", id);
    auto id_pos = json.find(needle);
    if (id_pos == std::string_view::npos) {
        return {};
    }
    auto object_start = json.rfind('{', id_pos);
    if (object_start == std::string_view::npos) {
        return {};
    }
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t pos = object_start; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(object_start, pos - object_start + 1);
            }
        }
    }
    return {};
}

std::string status_capability_summary(std::string_view object) {
    std::string summary;
    auto append = [&](std::string_view key, std::string_view label) {
        auto enabled = extract_bool_from(object, key);
        if (!enabled || !*enabled) {
            return;
        }
        if (!summary.empty()) {
            summary += "/";
        }
        summary += label;
    };
    append("audio", "audio");
    append("video", "video");
    append("app_lifecycle", "apps");
    append("media_control", "media");
    append("remote_control", "remote");
    append("metadata", "metadata");
    return summary;
}

std::string format_media_time(int64_t millis) {
    const auto total_seconds = millis <= 0 ? 0 : (millis + 500) / 1000;
    const auto hours = total_seconds / 3600;
    const auto minutes = (total_seconds % 3600) / 60;
    const auto seconds = total_seconds % 60;
    if (hours > 0) {
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
    }
    return std::format("{}:{:02}", minutes, seconds);
}

bool is_default_protocol_detail(std::string_view detail) {
    return detail == "disabled by config" || detail == "rtsp/raop receiver" ||
           detail == "cast v2 app/media receiver" ||
           detail == "wfd control/media lifecycle receiver";
}

}  // namespace

std::string render_status_json(const receiver_status_report& report) {
    std::ostringstream out;
    out << "{";
    out << "\"pid\":" << report.pid;
    out << ",\"name\":\"" << json_escape(report.name) << "\"";
    out << ",\"ip\":\"" << json_escape(report.ip) << "\"";
    out << ",\"interface\":\"" << json_escape(report.interface_name) << "\"";
    out << ",\"identity_key\":\"" << json_escape(report.identity_key) << "\"";
    out << ",\"airplay_port\":" << report.airplay_port;
    out << ",\"cast_port\":" << report.cast_port;
    out << ",\"miracast_port\":" << report.miracast_port;
    out << ",\"started\":" << report.started;
    out << ",\"protocols\":[";
    for (size_t i = 0; i < report.adapters.size(); ++i) {
        const auto& adapter = report.adapters[i];
        if (i > 0) {
            out << ",";
        }
        out << "{";
        out << "\"id\":\"" << protocol_id(adapter.id) << "\"";
        out << ",\"name\":\"" << to_string(adapter.id) << "\"";
        out << ",\"state\":\"" << to_string(adapter.state) << "\"";
        out << ",\"port\":" << adapter.port;
        out << ",\"advertised\":" << json_bool(adapter.advertised);
        out << ",\"experimental\":" << json_bool(adapter.experimental);
        out << ",\"detail\":\"" << json_escape(adapter.detail) << "\"";
        const auto source =
            std::ranges::find(report.sources, adapter.id, &receiver_source_descriptor::id);
        if (source != report.sources.end()) {
            const auto& caps = source->capabilities;
            out << ",\"transport\":\"" << json_escape(caps.transport) << "\"";
            out << ",\"capabilities\":{";
            out << "\"network_listener\":" << json_bool(caps.network_listener);
            out << ",\"discovery\":" << json_bool(caps.discovery);
            out << ",\"pairing\":" << json_bool(caps.pairing);
            out << ",\"media_setup\":" << json_bool(caps.media_setup);
            out << ",\"audio\":" << json_bool(caps.audio);
            out << ",\"video\":" << json_bool(caps.video);
            out << ",\"remote_control\":" << json_bool(caps.remote_control);
            out << ",\"app_lifecycle\":" << json_bool(caps.app_lifecycle);
            out << ",\"media_control\":" << json_bool(caps.media_control);
            out << ",\"metadata\":" << json_bool(caps.metadata);
            out << "}";
        }
        out << "}";
    }
    out << "]";
    out << ",\"clients\":[";
    for (size_t i = 0; i < report.clients.size(); ++i) {
        const auto& client = report.clients[i];
        if (i > 0) {
            out << ",";
        }
        out << "{";
        out << "\"id\":" << client.id;
        out << ",\"protocol\":\"" << protocol_id(client.protocol_id) << "\"";
        out << ",\"name\":\"" << json_escape(client.name) << "\"";
        out << ",\"address\":\"" << json_escape(client.address) << "\"";
        out << ",\"state\":\"" << json_escape(client.state) << "\"";
        out << ",\"connected_at\":" << client.connected_at;
        out << ",\"media\":{";
        out << "\"active\":" << json_bool(client.media.active);
        out << ",\"title\":\"" << json_escape(client.media.title) << "\"";
        out << ",\"artist\":\"" << json_escape(client.media.artist) << "\"";
        out << ",\"album\":\"" << json_escape(client.media.album) << "\"";
        out << ",\"artwork_type\":\"" << json_escape(client.media.artwork_type) << "\"";
        out << ",\"artwork_bytes\":" << client.media.artwork_bytes;
        out << ",\"position_ms\":" << client.media.position_ms;
        out << ",\"duration_ms\":" << client.media.duration_ms;
        out << ",\"volume_db\":" << client.media.volume_db;
        out << ",\"volume_linear\":" << client.media.volume_linear;
        out << "}";
        out << ",\"streams\":[";
        for (size_t stream_index = 0; stream_index < client.streams.size(); ++stream_index) {
            const auto& stream = client.streams[stream_index];
            if (stream_index > 0) {
                out << ",";
            }
            out << "{";
            out << "\"kind\":\"" << json_escape(stream.kind) << "\"";
            out << ",\"health\":\"" << json_escape(stream.health) << "\"";
            out << ",\"reason\":\"" << json_escape(stream.reason) << "\"";
            out << ",\"received_packets\":" << stream.received_packets;
            out << ",\"decoded_packets\":" << stream.decoded_packets;
            out << ",\"silent_or_marker\":" << stream.silent_or_marker;
            out << ",\"gaps\":" << stream.gaps;
            out << ",\"resend_requests\":" << stream.resend_requests;
            out << ",\"stale_or_redundant\":" << stream.stale_or_redundant;
            out << ",\"duplicates\":" << stream.duplicates;
            out << ",\"invalid\":" << stream.invalid;
            out << ",\"pending\":" << stream.pending;
            out << ",\"frames\":" << stream.frames;
            out << ",\"keyframes\":" << stream.keyframes;
            out << ",\"decrypted_frames\":" << stream.decrypted_frames;
            out << ",\"decrypt_failures\":" << stream.decrypt_failures;
            out << ",\"decode_failures\":" << stream.decode_failures;
            out << "}";
        }
        out << "]";
        out << "}";
    }
    out << "]";
    out << "}";
    return out.str();
}

receiver_status_summary parse_status_summary(std::string_view json) {
    receiver_status_summary summary;
    summary.name = extract_string_from(json, "name");
    summary.ip = extract_string_from(json, "ip");
    summary.interface_name = extract_string_from(json, "interface");
    summary.identity_key = extract_string_from(json, "identity_key");
    summary.airplay_port = extract_int_from(json, "airplay_port");
    summary.cast_port = extract_int_from(json, "cast_port");
    summary.miracast_port = extract_int_from(json, "miracast_port");
    summary.started = extract_int_from(json, "started");

    for (auto id : {"airplay", "cast", "miracast"}) {
        auto object = extract_protocol_object(json, id);
        if (object.empty()) {
            continue;
        }
        summary.protocols.push_back({
            .id = std::string(id),
            .state = extract_string_from(object, "state"),
            .detail = extract_string_from(object, "detail"),
            .transport = extract_string_from(object, "transport"),
            .capabilities = status_capability_summary(object),
            .port = extract_int_from(object, "port").value_or(0),
            .advertised = extract_bool_from(object, "advertised").value_or(false),
        });
    }

    for (auto object : extract_objects(extract_array_from(json, "clients"))) {
        receiver_status_client_summary client;
        client.protocol = extract_string_from(object, "protocol");
        client.address = extract_string_from(object, "address");
        client.state = extract_string_from(object, "state");
        client.name = extract_string_from(object, "name");

        if (auto media_object = extract_object_from(object, "media"); !media_object.empty()) {
            client.media = {
                .active = extract_bool_from(media_object, "active").value_or(false),
                .title = extract_string_from(media_object, "title"),
                .artist = extract_string_from(media_object, "artist"),
                .album = extract_string_from(media_object, "album"),
                .artwork_type = extract_string_from(media_object, "artwork_type"),
                .artwork_bytes = extract_int_from(media_object, "artwork_bytes").value_or(0),
                .position_ms = extract_int_from(media_object, "position_ms").value_or(0),
                .duration_ms = extract_int_from(media_object, "duration_ms").value_or(0),
            };
        }

        for (auto stream_object : extract_objects(extract_array_from(object, "streams"))) {
            client.streams.push_back({
                .kind = extract_string_from(stream_object, "kind"),
                .health = extract_string_from(stream_object, "health"),
                .reason = extract_string_from(stream_object, "reason"),
                .received_packets = extract_int_from(stream_object, "received_packets"),
                .decoded_packets = extract_int_from(stream_object, "decoded_packets"),
                .frames = extract_int_from(stream_object, "frames"),
                .keyframes = extract_int_from(stream_object, "keyframes"),
            });
        }

        summary.clients.push_back(std::move(client));
    }

    return summary;
}

std::string render_status_summary_text(const receiver_status_summary& summary, int pid,
                                       bool verbose,
                                       std::chrono::system_clock::time_point now) {
    std::ostringstream out;
    out << std::format("mirage is running (pid {})\n", pid);
    if (!summary.name.empty()) {
        out << std::format("  name: {}\n", summary.name);
    }
    if (!summary.ip.empty()) {
        if (!summary.interface_name.empty()) {
            out << std::format("  ip: {} ({})\n", summary.ip, summary.interface_name);
        } else {
            out << std::format("  ip: {}\n", summary.ip);
        }
    }
    if (!verbose && !summary.clients.empty()) {
        out << std::format("  clients: {}\n", summary.clients.size());
    }

    if (!verbose) {
        return out.str();
    }

    if (summary.airplay_port) {
        out << std::format("  airplay port: {}\n", *summary.airplay_port);
    }
    if (summary.cast_port) {
        out << std::format("  cast port: {}\n", *summary.cast_port);
    }
    if (summary.miracast_port) {
        out << std::format("  miracast port: {}\n", *summary.miracast_port);
    }
    if (!summary.identity_key.empty()) {
        out << std::format("  identity key: {}\n", summary.identity_key);
    }

    out << "  protocols:\n";
    for (const auto& protocol : summary.protocols) {
        const bool disabled = protocol.state == "disabled";
        std::string line = std::format("    {}: {}",
                                       protocol.id.empty() ? "protocol" : protocol.id,
                                       protocol.state.empty() ? "unknown" : protocol.state);
        if (protocol.port > 0) {
            line += std::format(", port {}", protocol.port);
        }
        if (!disabled && !protocol.transport.empty()) {
            line += std::format(", transport {}", protocol.transport);
        }
        if (!disabled && !protocol.capabilities.empty()) {
            line += std::format(", {}", protocol.capabilities);
        }
        if (!disabled && protocol.advertised) {
            line += ", advertised";
        }
        if (!protocol.detail.empty() && !is_default_protocol_detail(protocol.detail)) {
            line += std::format(", {}", protocol.detail);
        }
        out << line << '\n';
    }

    out << "  clients:\n";
    if (summary.clients.empty()) {
        out << "    none\n";
    }
    for (const auto& client : summary.clients) {
        std::string line =
            std::format("    {}", client.protocol.empty() ? "client" : client.protocol);
        if (!client.address.empty()) {
            line += std::format(": {}", client.address);
        }
        if (!client.state.empty()) {
            line += std::format(", {}", client.state);
        }
        if (!client.name.empty() && client.name != client.protocol) {
            line += std::format(", {}", client.name);
        }
        out << line << '\n';

        const auto& media = client.media;
        const bool has_media = media.active || !media.title.empty() || !media.artist.empty() ||
                               !media.album.empty() || media.artwork_bytes > 0 ||
                               media.position_ms > 0 || media.duration_ms > 0;
        if (has_media) {
            std::string media_line = "      media:";
            if (!media.title.empty()) {
                media_line += std::format(" {}", media.title);
                if (!media.artist.empty()) {
                    media_line += std::format(" - {}", media.artist);
                }
            } else if (!media.artist.empty()) {
                media_line += std::format(" {}", media.artist);
            } else {
                media_line += " active";
            }
            if (!media.album.empty()) {
                media_line += std::format(", album {}", media.album);
            }
            if (media.duration_ms > 0) {
                media_line += std::format(", {}/{}", format_media_time(media.position_ms),
                                          format_media_time(media.duration_ms));
            }
            if (media.artwork_bytes > 0) {
                media_line += ", artwork";
                if (!media.artwork_type.empty()) {
                    media_line += std::format(" {}", media.artwork_type);
                }
                media_line += std::format(" {} bytes", media.artwork_bytes);
            }
            out << media_line << '\n';
        }

        for (const auto& stream : client.streams) {
            std::string stream_line =
                std::format("      {}: {}", stream.kind.empty() ? "stream" : stream.kind,
                            stream.health.empty() ? "unknown" : stream.health);
            if (stream.kind == "audio") {
                if (stream.decoded_packets) {
                    stream_line += std::format(", decoded {}", *stream.decoded_packets);
                }
                if (stream.received_packets) {
                    stream_line += std::format(", received {}", *stream.received_packets);
                }
            } else if (stream.kind == "video") {
                if (stream.frames) {
                    stream_line += std::format(", frames {}", *stream.frames);
                }
                if (stream.keyframes) {
                    stream_line += std::format(", keyframes {}", *stream.keyframes);
                }
            }
            if (!stream.reason.empty() && stream.reason != "ok") {
                stream_line += std::format(", reason {}", stream.reason);
            }
            out << stream_line << '\n';
        }
    }

    if (summary.started) {
        const auto started_time =
            std::chrono::system_clock::from_time_t(static_cast<time_t>(*summary.started));
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_time);
        if (uptime.count() < 0) {
            uptime = std::chrono::seconds(0);
        }
        const auto hours = uptime.count() / 3600;
        const auto minutes = (uptime.count() % 3600) / 60;
        const auto secs = uptime.count() % 60;
        if (hours > 0) {
            out << std::format("  uptime: {}h {}m {}s\n", hours, minutes, secs);
        } else if (minutes > 0) {
            out << std::format("  uptime: {}m {}s\n", minutes, secs);
        } else {
            out << std::format("  uptime: {}s\n", secs);
        }
    }

    return out.str();
}

}  // namespace mirage
