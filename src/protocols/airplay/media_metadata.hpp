#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mirage::airplay {

struct dmap_media_metadata {
    std::string title;
    std::string artist;
    std::string album;

    [[nodiscard]] bool empty() const {
        return title.empty() && artist.empty() && album.empty();
    }
};

struct media_progress_status {
    uint64_t position_ms = 0;
    uint64_t duration_ms = 0;
};

[[nodiscard]] dmap_media_metadata parse_dmap_media_metadata(std::span<const std::byte> data);

[[nodiscard]] std::optional<media_progress_status> parse_airplay_progress(std::string_view value,
                                                                          int sample_rate);

}  // namespace mirage::airplay
