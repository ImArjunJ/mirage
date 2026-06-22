#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "protocols/airplay/media_source.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::byte> make_rtp(uint16_t seqnum, uint32_t timestamp,
                                std::vector<std::byte> payload) {
    std::vector<std::byte> packet(12 + payload.size());
    packet[0] = std::byte{0x80};
    packet[1] = std::byte{0x60};
    packet[2] = std::byte{static_cast<uint8_t>((seqnum >> 8) & 0xFF)};
    packet[3] = std::byte{static_cast<uint8_t>(seqnum & 0xFF)};
    packet[4] = std::byte{static_cast<uint8_t>((timestamp >> 24) & 0xFF)};
    packet[5] = std::byte{static_cast<uint8_t>((timestamp >> 16) & 0xFF)};
    packet[6] = std::byte{static_cast<uint8_t>((timestamp >> 8) & 0xFF)};
    packet[7] = std::byte{static_cast<uint8_t>(timestamp & 0xFF)};
    std::copy(payload.begin(), payload.end(), packet.begin() + 12);
    return packet;
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

    void on_audio_flush() override {}
    void on_audio_pause() override {}
    void on_audio_teardown() override {}
    void on_audio_volume(float /* db */, float /* linear */) override {}

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
    int video_setups = 0;
    int video_teardowns = 0;
    bool video_is_open = false;
};

}  // namespace

int main() {
    recording_sink sink;
    mirage::protocols::airplay::media_source source(sink);
    bool ok = true;

    auto configured = source.configure_audio({
        .codec_tag = 8,
        .sample_rate = 44100,
        .channels = 2,
        .frames_per_packet = 480,
    });
    ok &= expect(configured.has_value(), "audio source setup failed");
    ok &= expect(sink.audio_setups == 1, "audio setup count mismatch");
    ok &= expect(sink.audio_config.codec == mirage::audio_codec::aac, "audio codec mismatch");

    auto pkt10 = make_rtp(10, 1000, {std::byte{0x8c}, std::byte{0x01}});
    auto first = source.receive_audio_rtp(pkt10, false);
    ok &= expect(first.accepted, "first audio packet was not accepted");
    ok &= expect(!first.resend.has_value(), "first audio packet requested resend");
    ok &= expect(sink.audio_packets.size() == 1, "first audio packet was not delivered");
    ok &= expect(sink.audio_packets.back().sequence == 10, "first sequence mismatch");
    ok &= expect(sink.audio_packets.back().timestamp == 1000, "first timestamp mismatch");

    auto pkt12 = make_rtp(12, 1240, {std::byte{0x8d}, std::byte{0x02}});
    auto gap = source.receive_audio_rtp(pkt12, false);
    ok &= expect(gap.accepted, "gapped audio packet was not accepted");
    ok &= expect(gap.resend.has_value(), "gapped audio packet did not request resend");
    ok &= expect(gap.resend && gap.resend->start_seqnum == 11, "resend start mismatch");
    ok &= expect(gap.resend && gap.resend->count == 1, "resend count mismatch");
    ok &= expect(sink.audio_packets.size() == 1, "gapped packet was delivered too early");

    auto duplicate = source.receive_audio_rtp(pkt12, false);
    ok &= expect(!duplicate.accepted, "duplicate pending packet was accepted");
    ok &= expect(source.audio_stats().duplicates == 1, "duplicate count mismatch");

    auto pkt11 = make_rtp(11, 1120, {std::byte{0x8e}, std::byte{0x03}});
    auto recovered = source.receive_audio_rtp(pkt11, true);
    ok &= expect(recovered.accepted, "retransmitted audio packet was not accepted");
    ok &= expect(sink.audio_packets.size() == 3, "recovered packets were not drained");
    ok &= expect(sink.audio_packets[1].sequence == 11, "recovered sequence mismatch");
    ok &= expect(sink.audio_packets[1].retransmitted, "retransmitted flag mismatch");
    ok &= expect(sink.audio_packets[2].sequence == 12, "queued sequence mismatch");

    auto stale = source.receive_audio_rtp(pkt11, false);
    ok &= expect(!stale.accepted, "stale packet was accepted");
    ok &= expect(source.audio_stats().stale_or_redundant == 1, "stale count mismatch");

    auto marker =
        make_rtp(13, 1360, {std::byte{0x00}, std::byte{0x68}, std::byte{0x34}, std::byte{0x00}});
    auto marker_result = source.receive_audio_rtp(marker, false);
    ok &= expect(marker_result.accepted, "marker packet was not accepted");
    ok &= expect(source.audio_stats().silent_or_marker == 1, "marker count mismatch");
    ok &= expect(sink.audio_packets.size() == 3, "marker packet was delivered");

    auto invalid = make_rtp(14, 1480, {std::byte{0x20}, std::byte{0x04}});
    auto invalid_result = source.receive_audio_rtp(invalid, false);
    ok &= expect(invalid_result.accepted, "invalid packet was not queued");
    ok &= expect(source.audio_stats().invalid == 1, "invalid count mismatch");
    ok &= expect(sink.audio_packets.size() == 3, "invalid packet was delivered");

    auto stats = source.audio_stats();
    ok &= expect(stats.decoded_packets == 3, "decoded count mismatch");
    ok &= expect(stats.gaps == 1, "gap count mismatch");
    ok &= expect(stats.resend_requests == 1, "resend count mismatch");
    ok &= expect(stats.pending == 0, "pending count mismatch");

    source.reset_audio_packets();
    ok &= expect(source.audio_stats().decoded_packets == 0, "reset decoded count mismatch");
    ok &= expect(source.audio_stats().pending == 0, "reset pending count mismatch");

    auto video_started = source.start_video({
        .sink_config =
            {
                .codec = mirage::video_codec::h264,
                .width = 640,
                .height = 480,
                .prefer_hardware = false,
                .title = "test",
            },
        .stream_connection_id = 0,
        .keymsg = {},
        .ekey = {},
        .shared_secret = {},
    });
    ok &= expect(video_started.has_value(), "video source setup failed");
    ok &= expect(source.video_open(), "video source did not open");
    ok &= expect(sink.video_setups == 1, "video setup count mismatch");
    ok &= expect(sink.video_config.width == 640, "video width mismatch");

    std::array<std::byte, 1> encrypted_frame{std::byte{0x01}};
    auto video_frame = source.receive_mirror_payload(0x00, 0x10, 0x00, encrypted_frame);
    ok &= expect(video_frame.has_value(), "video frame handling failed");
    ok &= expect(source.video_stats().frames == 1, "video frame count mismatch");
    ok &= expect(source.video_stats().keyframes == 1, "video keyframe count mismatch");
    ok &= expect(sink.video_packets.empty(), "encrypted video packet was emitted without keys");

    source.stop_video();
    ok &= expect(!source.video_open(), "video source did not close");
    ok &= expect(sink.video_teardowns == 1, "video teardown count mismatch");

    return ok ? 0 : 1;
}
