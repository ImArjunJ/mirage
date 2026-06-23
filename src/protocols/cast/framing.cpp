#include "protocols/cast/framing.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace mirage::protocols::cast {
namespace {

uint32_t read_be32(std::span<const std::byte, channel_frame_header_size> header) {
    return (std::to_integer<uint32_t>(header[0]) << 24) |
           (std::to_integer<uint32_t>(header[1]) << 16) |
           (std::to_integer<uint32_t>(header[2]) << 8) |
           std::to_integer<uint32_t>(header[3]);
}

void write_be32(uint32_t value, std::span<std::byte, channel_frame_header_size> header) {
    header[0] = std::byte{static_cast<unsigned char>((value >> 24) & 0xff)};
    header[1] = std::byte{static_cast<unsigned char>((value >> 16) & 0xff)};
    header[2] = std::byte{static_cast<unsigned char>((value >> 8) & 0xff)};
    header[3] = std::byte{static_cast<unsigned char>(value & 0xff)};
}

uint32_t read_be32_prefix(std::string_view data) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
}

mirage_error frame_size_error(uint32_t length, uint32_t max_frame_size) {
    return mirage_error::cast_err(
        std::format("channel frame length {} exceeds limit {}", length, max_frame_size));
}

}  // namespace

channel_frame_parser::channel_frame_parser(uint32_t max_frame_size)
    : max_frame_size_(max_frame_size) {}

result<void> channel_frame_parser::append(std::span<const std::byte> data) {
    buffer_.insert(buffer_.end(), data.begin(), data.end());

    while (buffer_.size() >= channel_frame_header_size) {
        const auto length = read_be32(std::span<const std::byte, channel_frame_header_size>(
            buffer_.data(), channel_frame_header_size));
        if (length > max_frame_size_) {
            clear();
            return std::unexpected(frame_size_error(length, max_frame_size_));
        }
        if (buffer_.size() - channel_frame_header_size < length) {
            break;
        }

        auto payload_begin =
            buffer_.begin() + static_cast<std::ptrdiff_t>(channel_frame_header_size);
        auto payload_end = payload_begin + static_cast<std::ptrdiff_t>(length);
        frames_.emplace_back(payload_begin, payload_end);
        buffer_.erase(buffer_.begin(), payload_end);
    }

    return {};
}

std::optional<std::vector<std::byte>> channel_frame_parser::next_frame() {
    if (frames_.empty()) {
        return std::nullopt;
    }
    auto frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
}

void channel_frame_parser::clear() {
    buffer_.clear();
    frames_.clear();
}

bool looks_like_channel_frame(std::string_view data, uint32_t max_frame_size) {
    if (data.size() < channel_frame_header_size) {
        return false;
    }
    return read_be32_prefix(data) <= max_frame_size;
}

result<std::vector<std::byte>> make_channel_frame(std::span<const std::byte> payload,
                                                  uint32_t max_frame_size) {
    if (payload.size() > max_frame_size) {
        return std::unexpected(
            frame_size_error(static_cast<uint32_t>(payload.size()), max_frame_size));
    }

    std::vector<std::byte> frame(channel_frame_header_size + payload.size());
    write_be32(static_cast<uint32_t>(payload.size()),
               std::span<std::byte, channel_frame_header_size>(frame.data(),
                                                               channel_frame_header_size));
    std::copy(payload.begin(), payload.end(), frame.begin() + channel_frame_header_size);
    return frame;
}

}  // namespace mirage::protocols::cast
