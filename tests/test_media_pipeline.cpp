#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "media/pipeline.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class recording_sink final : public mirage::media::media_sink {
public:
    mirage::result<void> on_audio_setup(const mirage::media::audio_stream_config& config) override {
        audio_config = config;
        ++audio_setups;
        return {};
    }

    mirage::result<void> on_audio_packet(const mirage::media::media_packet& packet) override {
        audio_packets.push_back(packet);
        return {};
    }

    void on_audio_flush() override { ++audio_flushes; }

    void on_audio_pause() override { ++audio_pauses; }

    void on_audio_teardown() override { ++audio_teardowns; }

    void on_audio_volume(float db, float linear) override {
        volume_db = db;
        linear_volume = linear;
    }

    mirage::result<void> on_video_setup(const mirage::media::video_stream_config& config) override {
        video_config = config;
        video_is_open = true;
        ++video_setups;
        return {};
    }

    mirage::result<void> on_video_packet(const mirage::media::media_packet& packet) override {
        video_packets.push_back(packet);
        return {};
    }

    [[nodiscard]] bool video_open() const override { return video_is_open; }

    void on_video_teardown() override {
        video_is_open = false;
        ++video_teardowns;
    }

    mirage::media::audio_stream_config audio_config;
    mirage::media::video_stream_config video_config;
    std::vector<mirage::media::media_packet> audio_packets;
    std::vector<mirage::media::media_packet> video_packets;
    int audio_setups = 0;
    int audio_flushes = 0;
    int audio_pauses = 0;
    int audio_teardowns = 0;
    int video_setups = 0;
    int video_teardowns = 0;
    bool video_is_open = false;
    float volume_db = 0.0F;
    float linear_volume = 1.0F;
};

}  // namespace

int main() {
    recording_sink sink;
    bool ok = true;

    std::vector<std::byte> alac_cookie{std::byte{0x00}, std::byte{0x24}, std::byte{0x61}};
    auto audio_setup = sink.on_audio_setup({
        .codec = mirage::audio_codec::alac,
        .sample_rate = 48000,
        .channels = 2,
        .frames_per_packet = 352,
        .codec_tag = 2,
        .codec_config = alac_cookie,
    });
    ok &= expect(audio_setup.has_value(), "audio setup failed");
    ok &= expect(sink.audio_setups == 1, "audio setup count mismatch");
    ok &= expect(sink.audio_config.codec == mirage::audio_codec::alac, "audio codec mismatch");
    ok &= expect(sink.audio_config.sample_rate == 48000, "audio sample rate mismatch");
    ok &= expect(sink.audio_config.codec_config == alac_cookie, "audio config payload mismatch");

    sink.on_audio_volume(-12.0F, 0.25F);
    ok &= expect(sink.volume_db == -12.0F, "audio volume db mismatch");
    ok &= expect(sink.linear_volume == 0.25F, "audio linear volume mismatch");

    auto audio_packet = sink.on_audio_packet({
        .stream = mirage::media::media_stream_type::audio,
        .payload = {std::byte{0x11}, std::byte{0x22}},
        .sequence = 42,
        .timestamp = 1234,
        .keyframe = false,
        .retransmitted = true,
    });
    ok &= expect(audio_packet.has_value(), "audio packet failed");
    ok &= expect(sink.audio_packets.size() == 1, "audio packet count mismatch");
    ok &= expect(sink.audio_packets.front().sequence == 42, "audio packet sequence mismatch");
    ok &= expect(sink.audio_packets.front().timestamp == 1234, "audio packet timestamp mismatch");
    ok &= expect(sink.audio_packets.front().retransmitted, "audio retransmit flag mismatch");

    sink.on_audio_flush();
    sink.on_audio_pause();
    sink.on_audio_teardown();
    ok &= expect(sink.audio_flushes == 1, "audio flush count mismatch");
    ok &= expect(sink.audio_pauses == 1, "audio pause count mismatch");
    ok &= expect(sink.audio_teardowns == 1, "audio teardown count mismatch");

    auto video_setup = sink.on_video_setup({
        .codec = mirage::video_codec::h264,
        .width = 1920,
        .height = 1080,
        .prefer_hardware = false,
        .title = "test",
    });
    ok &= expect(video_setup.has_value(), "video setup failed");
    ok &= expect(sink.video_open(), "video did not open");
    ok &= expect(sink.video_config.width == 1920, "video width mismatch");
    ok &= expect(sink.video_config.height == 1080, "video height mismatch");
    ok &= expect(!sink.video_config.prefer_hardware, "video hardware preference mismatch");

    auto video_packet = sink.on_video_packet({
        .stream = mirage::media::media_stream_type::video,
        .payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x01}},
        .sequence = 7,
        .timestamp = 99,
        .keyframe = true,
        .retransmitted = false,
    });
    ok &= expect(video_packet.has_value(), "video packet failed");
    ok &= expect(sink.video_packets.size() == 1, "video packet count mismatch");
    ok &= expect(sink.video_packets.front().keyframe, "video keyframe flag mismatch");

    sink.on_video_teardown();
    ok &= expect(!sink.video_open(), "video did not close");
    ok &= expect(sink.video_teardowns == 1, "video teardown count mismatch");

    mirage::media::stream_event event{
        .type = mirage::media::stream_event_type::packet,
        .stream = mirage::media::media_stream_type::audio,
        .sequence = 42,
    };
    ok &= expect(event.sequence == 42, "stream event sequence mismatch");

    return ok ? 0 : 1;
}
