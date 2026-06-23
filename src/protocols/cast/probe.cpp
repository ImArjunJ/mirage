#include "protocols/cast/probe.hpp"

#include <format>
#include <string>
#include <string_view>

#include "protocols/cast/framing.hpp"

namespace mirage::protocols::cast {
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

bool starts_with_http_method(std::string_view data, std::string_view method) {
    return data.starts_with(method) && data.size() > method.size() && data[method.size()] == ' ';
}

std::string http_response(int status, std::string_view reason, std::string_view body,
                          bool include_body) {
    std::string response =
        std::format("HTTP/1.1 {} {}\r\n"
                    "Connection: close\r\n"
                    "Cache-Control: no-store\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: {}\r\n"
                    "\r\n",
                    status, reason, include_body ? body.size() : 0);
    if (include_body) {
        response += body;
    }
    return response;
}

std::string options_response() {
    return "HTTP/1.1 204 No Content\r\n"
           "Connection: close\r\n"
           "Allow: GET, HEAD, OPTIONS\r\n"
           "Content-Length: 0\r\n"
           "\r\n";
}

bool is_tls_client_hello(std::string_view data) {
    return data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0x16 &&
           static_cast<unsigned char>(data[1]) == 0x03;
}

}  // namespace

probe_response handle_probe(std::string_view data, std::string_view device_name) {
    if (is_tls_client_hello(data)) {
        return {.kind = probe_kind::tls_client_hello, .response = {}};
    }

    if (starts_with_http_method(data, "OPTIONS")) {
        return {.kind = probe_kind::http_options, .response = options_response()};
    }

    const bool is_head = starts_with_http_method(data, "HEAD");
    if (starts_with_http_method(data, "GET") || starts_with_http_method(data, "POST") ||
        is_head) {
        auto body = std::format(
            "{{\"name\":\"{}\",\"receiver\":\"cast-v2\","
            "\"status\":\"app_media_ready\","
            "\"detail\":\"cast app/media control channel is available; media rendering is not "
            "implemented yet\"}}\n",
            json_escape(device_name));
        return {.kind = probe_kind::http_status,
                .response = http_response(200, "OK", body, !is_head)};
    }

    if (looks_like_channel_frame(data)) {
        return {.kind = probe_kind::channel_frame, .response = {}};
    }

    return {.kind = probe_kind::unsupported, .response = {}};
}

}  // namespace mirage::protocols::cast
