#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/core.hpp"
#include "protocols/rtsp_message.hpp"

namespace mirage::protocols::wfd {

inline constexpr std::string_view public_methods =
    "org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER, OPTIONS, TEARDOWN";

enum class set_parameter_result : uint8_t {
    accepted,
    client_rtp_ports,
    media_trigger,
    unsupported_parameter,
};

struct client_rtp_ports {
    std::string profile;
    std::string delivery;
    uint16_t primary_port = 0;
    uint16_t secondary_port = 0;
    std::string mode;
};

struct set_parameter_analysis {
    set_parameter_result result = set_parameter_result::accepted;
    std::string parameter;
    std::string value;
    std::optional<client_rtp_ports> rtp_ports;
};

enum class control_event : uint8_t {
    none,
    parameters_accepted,
    client_rtp_ports_configured,
    media_trigger_requested,
    media_method_requested,
    unsupported_parameter,
    teardown_requested,
};

struct control_session_state {
    std::optional<std::string> pending_trigger;
    std::optional<client_rtp_ports> client_ports;
    uint64_t accepted_parameter_sets = 0;
    uint64_t client_rtp_port_updates = 0;
    uint64_t media_triggers = 0;
    uint64_t media_methods = 0;
    uint64_t unsupported_parameters = 0;
    bool teardown_requested = false;
};

struct control_response {
    int status_code = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool close_after_send = false;
    control_event event = control_event::none;
    std::string event_detail;
};

[[nodiscard]] std::string capability_text(std::string_view requested_parameters = {});
[[nodiscard]] set_parameter_analysis analyze_set_parameters(std::string_view body);
[[nodiscard]] result<control_response> handle_control_request(const rtsp_request_head& request,
                                                              std::string_view body);
[[nodiscard]] result<control_response> handle_control_request(const rtsp_request_head& request,
                                                              std::string_view body,
                                                              control_session_state& state);
[[nodiscard]] std::string serialize_control_response(const control_response& response,
                                                     std::optional<uint32_t> cseq);

}  // namespace mirage::protocols::wfd
