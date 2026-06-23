#include "protocols/airplay/media_metadata.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace mirage::airplay {
namespace {

std::optional<uint32_t> read_be32(std::span<const std::byte> data, size_t offset) {
    if (offset + 4 > data.size()) {
        return std::nullopt;
    }

    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
        value = (value << 8) | static_cast<uint32_t>(
                                   static_cast<uint8_t>(data[offset + index]));
    }
    return value;
}

bool is_printable_fourcc(std::span<const std::byte> data, size_t offset) {
    if (offset + 4 > data.size()) {
        return false;
    }
    return std::ranges::all_of(data.subspan(offset, 4), [](std::byte value) {
        const auto c = static_cast<unsigned char>(static_cast<uint8_t>(value));
        return std::isalnum(c) != 0 || c == '_';
    });
}

bool looks_like_dmap_stream(std::span<const std::byte> data) {
    if (data.size() < 8 || !is_printable_fourcc(data, 0)) {
        return false;
    }
    auto length = read_be32(data, 4);
    if (!length) {
        return false;
    }
    const auto payload_len = static_cast<size_t>(*length);
    return payload_len <= data.size() - 8;
}

std::string clean_dmap_text(std::span<const std::byte> data) {
    std::string value;
    value.reserve(data.size());
    for (auto byte : data) {
        const auto c = static_cast<unsigned char>(static_cast<uint8_t>(byte));
        if (c == 0 || (c < 0x20 && c != '\t')) {
            continue;
        }
        value.push_back(static_cast<char>(c));
    }
    return value;
}

std::optional<std::string> find_dmap_string(std::span<const std::byte> data,
                                            std::string_view wanted,
                                            size_t depth = 0) {
    if (depth > 8) {
        return std::nullopt;
    }

    size_t offset = 0;
    while (offset + 8 <= data.size()) {
        if (!is_printable_fourcc(data, offset)) {
            break;
        }
        auto length = read_be32(data, offset + 4);
        if (!length) {
            break;
        }
        const auto payload_len = static_cast<size_t>(*length);
        const auto payload_start = offset + 8;
        if (payload_len > data.size() - payload_start) {
            break;
        }

        auto tag = std::string_view(reinterpret_cast<const char*>(data.data() + offset), 4);
        auto payload = data.subspan(payload_start, payload_len);
        if (tag == wanted) {
            return clean_dmap_text(payload);
        }
        if (looks_like_dmap_stream(payload)) {
            if (auto nested = find_dmap_string(payload, wanted, depth + 1)) {
                return nested;
            }
        }

        offset = payload_start + payload_len;
    }
    return std::nullopt;
}

std::optional<uint64_t> parse_u64(std::string_view value, size_t& pos) {
    while (pos < value.size() &&
           std::isspace(static_cast<unsigned char>(value[pos])) != 0) {
        ++pos;
    }
    if (pos >= value.size() ||
        std::isdigit(static_cast<unsigned char>(value[pos])) == 0) {
        return std::nullopt;
    }

    uint64_t parsed = 0;
    while (pos < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[pos])) != 0) {
        const auto digit = static_cast<uint64_t>(value[pos] - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        parsed = parsed * 10 + digit;
        ++pos;
    }
    return parsed;
}

std::optional<uint64_t> ticks_to_ms(uint64_t ticks, int sample_rate) {
    if (sample_rate <= 0) {
        return std::nullopt;
    }
    const auto ms = (static_cast<long double>(ticks) * 1000.0L) /
                    static_cast<long double>(sample_rate);
    if (ms > static_cast<long double>(std::numeric_limits<uint64_t>::max())) {
        return std::nullopt;
    }
    return static_cast<uint64_t>(ms);
}

}  // namespace

dmap_media_metadata parse_dmap_media_metadata(std::span<const std::byte> data) {
    dmap_media_metadata metadata;
    if (auto title = find_dmap_string(data, "minm")) {
        metadata.title = std::move(*title);
    }
    if (auto artist = find_dmap_string(data, "asar")) {
        metadata.artist = std::move(*artist);
    }
    if (auto album = find_dmap_string(data, "asal")) {
        metadata.album = std::move(*album);
    }
    return metadata;
}

std::optional<media_progress_status> parse_airplay_progress(std::string_view value,
                                                            int sample_rate) {
    auto pos = value.find("progress:");
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    pos += std::string_view("progress:").size();

    auto start = parse_u64(value, pos);
    if (!start || pos >= value.size() || value[pos] != '/') {
        return std::nullopt;
    }
    ++pos;
    auto current = parse_u64(value, pos);
    if (!current || pos >= value.size() || value[pos] != '/') {
        return std::nullopt;
    }
    ++pos;
    auto end = parse_u64(value, pos);
    if (!end || *current < *start || *end < *start) {
        return std::nullopt;
    }

    const auto position_ticks = *current - *start;
    const auto duration_ticks = *end - *start;
    auto position_ms = ticks_to_ms(position_ticks, sample_rate);
    auto duration_ms = ticks_to_ms(duration_ticks, sample_rate);
    if (!position_ms || !duration_ms) {
        return std::nullopt;
    }

    return media_progress_status{
        .position_ms = *position_ms,
        .duration_ms = *duration_ms,
    };
}

}  // namespace mirage::airplay
