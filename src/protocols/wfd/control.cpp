#include "protocols/wfd/control.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace mirage::protocols::wfd {
namespace {

struct parameter_value {
    std::string_view name;
    std::string_view value;
};

struct parsed_parameter {
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

std::vector<parsed_parameter> parse_parameter_lines(std::string_view body) {
    std::vector<parsed_parameter> parameters;
    while (!body.empty()) {
        auto newline = body.find('\n');
        auto line = newline == std::string_view::npos ? body : body.substr(0, newline);
        body.remove_prefix(newline == std::string_view::npos ? body.size() : newline + 1);
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            parameters.push_back({.name = line, .value = {}});
            continue;
        }
        parameters.push_back({
            .name = trim(line.substr(0, colon)),
            .value = trim(line.substr(colon + 1)),
        });
    }
    return parameters;
}

bool requested(std::span<const std::string_view> names, std::string_view capability_name) {
    return names.empty() ||
           std::ranges::find(names, capability_name) != names.end();
}

bool equals_ascii_case(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::ranges::equal(lhs, rhs, [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

bool has_capability(std::string_view name) {
    return std::ranges::any_of(capabilities, [name](const parameter_value& capability) {
        return capability.name == name;
    });
}

bool known_set_parameter(std::string_view name) {
    return has_capability(name) || name == "wfd_trigger_method" ||
           name.starts_with("wfd_");
}

bool media_trigger_value(std::string_view value) {
    return equals_ascii_case(value, "SETUP") || equals_ascii_case(value, "PLAY") ||
           equals_ascii_case(value, "PAUSE") || equals_ascii_case(value, "RECORD");
}

std::optional<uint16_t> parse_u16(std::string_view value) {
    uint32_t parsed = 0;
    const auto* first = value.data();
    const auto* last = value.data() + value.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc{} || ptr != last ||
        parsed > std::numeric_limits<uint16_t>::max()) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(parsed);
}

std::optional<client_rtp_ports> parse_client_rtp_ports(std::string_view value) {
    std::istringstream in{std::string(value)};
    std::string transport_token;
    std::string primary_token;
    std::string secondary_token;
    if (!(in >> transport_token >> primary_token >> secondary_token)) {
        return std::nullopt;
    }

    auto primary = parse_u16(primary_token);
    auto secondary = parse_u16(secondary_token);
    if (!primary || !secondary) {
        return std::nullopt;
    }

    client_rtp_ports ports;
    const auto separator = transport_token.find(';');
    ports.profile = separator == std::string::npos
                        ? transport_token
                        : transport_token.substr(0, separator);
    ports.delivery = separator == std::string::npos
                         ? std::string{}
                         : transport_token.substr(separator + 1);
    ports.primary_port = *primary;
    ports.secondary_port = *secondary;

    std::string token;
    while (in >> token) {
        constexpr std::string_view mode_prefix = "mode=";
        if (token.starts_with(mode_prefix)) {
            ports.mode = token.substr(mode_prefix.size());
        }
    }

    return ports;
}

control_response make_simple_response(int code, std::string text) {
    control_response response;
    response.status_code = code;
    response.status_text = std::move(text);
    response.headers.emplace_back("Public", std::string(public_methods));
    return response;
}

control_response media_not_implemented(std::string_view method,
                                       const std::optional<std::string>& pending_trigger) {
    auto response = make_simple_response(501, "Media Not Implemented");
    response.body = "wfd_error: media-not-implemented\r\n";
    if (!method.empty()) {
        response.body += "method: ";
        response.body += method;
        response.body += "\r\n";
    }
    if (pending_trigger && !pending_trigger->empty()) {
        response.body += "trigger: ";
        response.body += *pending_trigger;
        response.body += "\r\n";
    }
    response.headers.emplace_back("Content-Type", "text/parameters");
    response.event = control_event::media_method_requested;
    response.event_detail = std::string(method);
    return response;
}

control_response parameter_not_understood(const set_parameter_analysis& analysis) {
    auto response = make_simple_response(451, "Parameter Not Understood");
    response.body = "wfd_error: parameter-not-understood\r\n";
    if (!analysis.parameter.empty()) {
        response.body += "parameter: ";
        response.body += analysis.parameter;
        response.body += "\r\n";
    }
    response.headers.emplace_back("Content-Type", "text/parameters");
    response.event = control_event::unsupported_parameter;
    response.event_detail = analysis.parameter;
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

set_parameter_analysis analyze_set_parameters(std::string_view body) {
    for (const auto& parameter : parse_parameter_lines(body)) {
        if (parameter.name.empty() || parameter.value.empty() ||
            !known_set_parameter(parameter.name)) {
            return {
                .result = set_parameter_result::unsupported_parameter,
                .parameter = std::string(parameter.name),
                .value = std::string(parameter.value),
                .rtp_ports = std::nullopt,
            };
        }

        if (parameter.name == "wfd_client_rtp_ports") {
            auto ports = parse_client_rtp_ports(parameter.value);
            if (!ports) {
                return {
                    .result = set_parameter_result::unsupported_parameter,
                    .parameter = std::string(parameter.name),
                    .value = std::string(parameter.value),
                    .rtp_ports = std::nullopt,
                };
            }
            return {
                .result = set_parameter_result::client_rtp_ports,
                .parameter = std::string(parameter.name),
                .value = std::string(parameter.value),
                .rtp_ports = std::move(ports),
            };
        }

        if (parameter.name == "wfd_trigger_method" &&
            media_trigger_value(parameter.value)) {
            return {
                .result = set_parameter_result::media_trigger,
                .parameter = std::string(parameter.name),
                .value = std::string(parameter.value),
                .rtp_ports = std::nullopt,
            };
        }
    }

    return {};
}

result<control_response> handle_control_request(const rtsp_request_head& request,
                                                std::string_view body) {
    control_session_state state;
    return handle_control_request(request, body, state);
}

result<control_response> handle_control_request(const rtsp_request_head& request,
                                                std::string_view body,
                                                control_session_state& state) {
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
        const auto analysis = analyze_set_parameters(body);
        if (analysis.result == set_parameter_result::media_trigger) {
            ++state.media_triggers;
            state.pending_trigger = analysis.value;
            auto response = make_simple_response(200, "OK");
            response.headers.emplace_back("Supported", "org.wfa.wfd1.0");
            response.event = control_event::media_trigger_requested;
            response.event_detail = analysis.value;
            return response;
        }
        if (analysis.result == set_parameter_result::client_rtp_ports) {
            ++state.accepted_parameter_sets;
            ++state.client_rtp_port_updates;
            state.client_ports = analysis.rtp_ports;
            auto response = make_simple_response(200, "OK");
            response.headers.emplace_back("Supported", "org.wfa.wfd1.0");
            response.event = control_event::client_rtp_ports_configured;
            if (state.client_ports) {
                response.event_detail = std::to_string(state.client_ports->primary_port);
            }
            return response;
        }
        if (analysis.result == set_parameter_result::unsupported_parameter) {
            ++state.unsupported_parameters;
            return parameter_not_understood(analysis);
        }
        ++state.accepted_parameter_sets;
        auto response = make_simple_response(200, "OK");
        response.headers.emplace_back("Supported", "org.wfa.wfd1.0");
        response.event = control_event::parameters_accepted;
        return response;
    }

    if (request.method == "TEARDOWN") {
        auto response = make_simple_response(200, "OK");
        response.close_after_send = true;
        response.event = control_event::teardown_requested;
        state.teardown_requested = true;
        return response;
    }

    if (request.method == "SETUP" || request.method == "PLAY" || request.method == "PAUSE" ||
        request.method == "RECORD") {
        ++state.media_methods;
        return media_not_implemented(request.method, state.pending_trigger);
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
