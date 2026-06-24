#pragma once

#include <memory>
#include <string>

#include "core/core.hpp"

namespace mirage::media {

struct remote_media_load {
    std::string url;
    std::string content_type;
    std::string title;
    std::string artist;
    std::string album;
    double start_time = 0.0;
    double duration = 0.0;
    bool autoplay = true;
};

struct remote_media_playback_status {
    bool active = false;
    bool audio = false;
    bool video = false;
    std::string detail;
};

class url_media_player {
public:
    url_media_player();
    ~url_media_player();

    url_media_player(const url_media_player&) = delete;
    url_media_player& operator=(const url_media_player&) = delete;

    void load(remote_media_load request);
    void stop();
    void set_volume(float db, float linear);

    [[nodiscard]] remote_media_playback_status status() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace mirage::media
