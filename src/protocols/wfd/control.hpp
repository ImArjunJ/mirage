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

struct control_response {
    int status_code = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool close_after_send = false;
};

[[nodiscard]] std::string capability_text(std::string_view requested_parameters = {});
[[nodiscard]] result<control_response> handle_control_request(const rtsp_request_head& request,
                                                              std::string_view body);
[[nodiscard]] std::string serialize_control_response(const control_response& response,
                                                     std::optional<uint32_t> cseq);

}  // namespace mirage::protocols::wfd
