#include "core/status_report.hpp"

#include <algorithm>
#include <sstream>
#include <string>

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

}  // namespace mirage
