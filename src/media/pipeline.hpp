#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/core.hpp"

namespace mirage::media {

enum class media_stream_type : uint8_t { audio, video };

enum class stream_event_type : uint8_t { setup, packet, flush, pause, teardown, volume };

struct audio_stream_config {
    audio_codec codec = audio_codec::aac;
    int sample_rate = 44100;
    int channels = 2;
    int frames_per_packet = 0;
    uint8_t codec_tag = 0;
    std::vector<std::byte> codec_config;
};

struct video_stream_config {
    video_codec codec = video_codec::h264;
    uint32_t width = 1280;
    uint32_t height = 720;
    bool prefer_hardware = true;
    std::string title;
};

struct media_packet {
    media_stream_type stream = media_stream_type::audio;
    std::vector<std::byte> payload;
    uint64_t sequence = 0;
    uint64_t timestamp = 0;
    bool keyframe = false;
    bool retransmitted = false;
};

struct stream_event {
    stream_event_type type = stream_event_type::packet;
    media_stream_type stream = media_stream_type::audio;
    uint64_t sequence = 0;
};

class media_sink {
public:
    virtual ~media_sink() = default;

    virtual result<void> on_audio_setup(const audio_stream_config& config) = 0;
    virtual result<void> on_audio_packet(const media_packet& packet) = 0;
    virtual void on_audio_flush() = 0;
    virtual void on_audio_pause() = 0;
    virtual void on_audio_teardown() = 0;
    virtual void on_audio_volume(float db, float linear) = 0;

    virtual result<void> on_video_setup(const video_stream_config& config) = 0;
    virtual result<void> on_video_packet(const media_packet& packet) = 0;
    [[nodiscard]] virtual bool video_open() const = 0;
    virtual void on_video_teardown() = 0;
};

std::unique_ptr<media_sink> make_local_media_sink();

}  // namespace mirage::media
