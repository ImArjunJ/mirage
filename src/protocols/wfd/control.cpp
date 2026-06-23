#include "protocols/wfd/control.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <sstream>
#include <utility>

namespace mirage::protocols::wfd {
namespace {

struct parameter_value {
    std::string_view name;
    std::string_view value;
};

constexpr std::array capabilities{
    parameter_value{"wfd_audio_codecs", "LPCM 00000003 00, AAC 0000000F 00"},
    parameter_value{"wfd_video_formats",
                    "00 00 02 10 0001FFFF 1FFFFFFF 00000FFF 00 0000 0000 00 none none"},
    parameter_value{"wfd_client_rtp_ports", "RTP/AVP/UDP;unicast 0 0 mode=play"},
    parameter_value{"wfd_content_protection", "none"},
    parameter_value{"wfd_display_edid", "none"},
    parameter_value{"wfd_connector_type", "05"},
    parameter_value{"wfd_uibc_capability", "none"},
    parameter_value{"wfd_standby_resume_capability", "none"},
};

std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    return value;
}

std::vector<std::string_view> requested_names(std::string_view request_body) {
    std::vector<std::string_view> names;
    while (!request_body.empty()) {
        auto newline = request_body.find('\n');
        auto line = newline == std::string_view::npos ? request_body
                                                      : request_body.substr(0, newline);
        request_body.remove_prefix(newline == std::string_view::npos ? request_body.size()
                                                                     : newline + 1);
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (auto colon = line.find(':'); colon != std::string_view::npos) {
            line = trim(line.substr(0, colon));
        }
        if (!line.empty()) {
            names.push_back(line);
        }
    }
    return names;
}

bool requested(std::span<const std::string_view> names, std::string_view capability_name) {
    return names.empty() ||
           std::ranges::find(names, capability_name) != names.end();
}

control_response make_simple_response(int code, std::string text) {
    control_response response;
    response.status_code = code;
    response.status_text = std::move(text);
    response.headers.emplace_back("Public", std::string(public_methods));
    return response;
}

control_response media_not_implemented() {
    auto response = make_simple_response(501, "Media Not Implemented");
    response.body = "wfd_error: media-not-implemented\r\n";
    response.headers.emplace_back("Content-Type", "text/parameters");
    return response;
}

}  // namespace

std::string capability_text(std::string_view requested_parameters) {
    auto names = requested_names(requested_parameters);
    std::string body;
    for (const auto& capability : capabilities) {
        if (!requested(names, capability.name)) {
            continue;
        }
        body += capability.name;
        body += ": ";
        body += capability.value;
        body += "\r\n";
    }
    return body;
}

result<control_response> handle_control_request(const rtsp_request_head& request,
                                                std::string_view body) {
    if (request.version != "RTSP/1.0") {
        return std::unexpected(mirage_error::network("unsupported WFD RTSP version"));
    }

    if (request.method == "OPTIONS") {
        auto response = make_simple_response(200, "OK");
        response.headers.emplace_back("Supported", "org.wfa.wfd1.0");
        return response;
    }

    if (request.method == "GET_PARAMETER") {
        auto response = make_simple_response(200, "OK");
        response.body = capability_text(body);
        response.headers.emplace_back("Content-Type", "text/parameters");
        return response;
    }

    if (request.method == "SET_PARAMETER") {
        auto response = make_simple_response(200, "OK");
        response.headers.emplace_back("Supported", "org.wfa.wfd1.0");
        return response;
    }

    if (request.method == "TEARDOWN") {
        auto response = make_simple_response(200, "OK");
        response.close_after_send = true;
        return response;
    }

    if (request.method == "SETUP" || request.method == "PLAY" || request.method == "PAUSE" ||
        request.method == "RECORD") {
        return media_not_implemented();
    }

    return make_simple_response(405, "Method Not Allowed");
}

std::string serialize_control_response(const control_response& response,
                                       std::optional<uint32_t> cseq) {
    std::ostringstream out;
    out << "RTSP/1.0 " << response.status_code << ' ' << response.status_text << "\r\n";
    out << "Server: Mirage/WFD\r\n";
    if (cseq) {
        out << "CSeq: " << *cseq << "\r\n";
    }
    for (const auto& [key, value] : response.headers) {
        out << key << ": " << value << "\r\n";
    }
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "\r\n";
    out << response.body;
    return out.str();
}

}  // namespace mirage::protocols::wfd
