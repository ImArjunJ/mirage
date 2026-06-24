#pragma once

#include <cstdint>
#include <functional>
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
    bool audio_output = false;
    uint64_t decoded_audio_frames = 0;
    uint64_t decoded_video_frames = 0;
    std::string detail;
};

using remote_media_status_callback = std::function<void(remote_media_playback_status)>;

class url_media_player {
public:
    url_media_player();
    ~url_media_player();

    url_media_player(const url_media_player&) = delete;
    url_media_player& operator=(const url_media_player&) = delete;

    void load(remote_media_load request);
    void stop();
    void play();
    void pause();
    void seek(double seconds);
    void set_playback_rate(double rate);
    void set_volume(float db, float linear);
    void set_status_callback(remote_media_status_callback callback);

    [[nodiscard]] remote_media_playback_status status() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace mirage::media
