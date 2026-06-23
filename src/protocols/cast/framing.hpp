#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"

namespace mirage::protocols::cast {

inline constexpr uint32_t default_channel_frame_max_size = 1024 * 1024;
inline constexpr size_t channel_frame_header_size = 4;

class channel_frame_parser {
public:
    explicit channel_frame_parser(uint32_t max_frame_size = default_channel_frame_max_size);

    [[nodiscard]] result<void> append(std::span<const std::byte> data);
    [[nodiscard]] std::optional<std::vector<std::byte>> next_frame();
    [[nodiscard]] size_t buffered_bytes() const { return buffer_.size(); }
    [[nodiscard]] size_t pending_frames() const { return frames_.size(); }
    void clear();

private:
    uint32_t max_frame_size_;
    std::vector<std::byte> buffer_;
    std::deque<std::vector<std::byte>> frames_;
};

[[nodiscard]] bool looks_like_channel_frame(
    std::string_view data, uint32_t max_frame_size = default_channel_frame_max_size);

[[nodiscard]] result<std::vector<std::byte>> make_channel_frame(
    std::span<const std::byte> payload,
    uint32_t max_frame_size = default_channel_frame_max_size);

}  // namespace mirage::protocols::cast
