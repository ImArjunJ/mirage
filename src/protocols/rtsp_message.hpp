#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/core.hpp"

namespace mirage::protocols {

inline constexpr size_t max_rtsp_body_bytes = 64U * 1024U * 1024U;

struct rtsp_request_head {
    std::string method;
    std::string uri;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::optional<uint32_t> cseq;
    size_t content_length = 0;
};

result<rtsp_request_head> parse_rtsp_request_head(std::string_view header_block);
std::optional<std::string_view> find_rtsp_header(
    const std::unordered_map<std::string, std::string>& headers, std::string_view name);

}  // namespace mirage::protocols
