#include "protocols/rtsp_message.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <limits>
#include <sstream>
#include <system_error>

namespace mirage::protocols {
namespace {

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

bool equal_header_name(std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

result<size_t> parse_size_header(std::string_view value, std::string_view header_name) {
    value = trim(value);
    if (value.empty()) {
        return std::unexpected(
            mirage_error::network(std::format("{} header is empty", header_name)));
    }

    size_t parsed = 0;
    auto* begin = value.data();
    auto* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(
            mirage_error::network(std::format("{} header is invalid", header_name)));
    }
    return parsed;
}

}  // namespace

std::optional<std::string_view> find_rtsp_header(
    const std::unordered_map<std::string, std::string>& headers, std::string_view name) {
    for (const auto& [key, value] : headers) {
        if (equal_header_name(key, name)) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

result<rtsp_request_head> parse_rtsp_request_head(std::string_view header_block) {
    rtsp_request_head request;

    std::istringstream stream{std::string(header_block)};
    std::string line;
    if (!std::getline(stream, line)) {
        return std::unexpected(mirage_error::network("empty RTSP request"));
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    if (!(request_line >> request.method >> request.uri >> request.version)) {
        return std::unexpected(mirage_error::network("invalid RTSP request line"));
    }
    if (request.method.empty() || request.uri.empty() || request.version.empty()) {
        return std::unexpected(mirage_error::network("invalid RTSP request line"));
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        auto key = trim(std::string_view(line).substr(0, colon));
        auto value = trim(std::string_view(line).substr(colon + 1));
        if (!key.empty()) {
            request.headers[std::string(key)] = std::string(value);
        }
    }

    if (auto cseq_value = find_rtsp_header(request.headers, "CSeq")) {
        auto parsed = parse_size_header(*cseq_value, "CSeq");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(mirage_error::network("CSeq header is too large"));
        }
        request.cseq = static_cast<uint32_t>(*parsed);
    }

    if (auto length_value = find_rtsp_header(request.headers, "Content-Length")) {
        auto parsed = parse_size_header(*length_value, "Content-Length");
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed > max_rtsp_body_bytes) {
            return std::unexpected(mirage_error::network("Content-Length header is too large"));
        }
        request.content_length = *parsed;
    }

    return request;
}

}  // namespace mirage::protocols
