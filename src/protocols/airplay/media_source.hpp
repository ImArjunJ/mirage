#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "crypto/crypto.hpp"
#include "media/pipeline.hpp"

namespace mirage::protocols::airplay {

struct audio_source_config {
    uint8_t codec_tag = 0;
    int sample_rate = 44100;
    int channels = 2;
    int frames_per_packet = 352;
};

struct audio_resend_request {
    uint16_t start_seqnum = 0;
    uint16_t count = 0;
};

struct audio_receive_result {
    bool accepted = false;
    std::optional<audio_resend_request> resend;
};

struct audio_source_stats {
    uint64_t decoded_packets = 0;
    uint64_t silent_or_marker = 0;
    uint64_t gaps = 0;
    uint64_t resend_requests = 0;
    uint64_t stale_or_redundant = 0;
    uint64_t duplicates = 0;
    uint64_t invalid = 0;
    size_t pending = 0;
};

struct video_source_config {
    media::video_stream_config sink_config;
    uint64_t stream_connection_id = 0;
    std::span<const std::byte> keymsg;
    std::span<const std::byte> ekey;
    std::span<const std::byte> shared_secret;
};

struct video_source_stats {
    uint64_t frames = 0;
    uint64_t keyframes = 0;
};

class media_source {
public:
    explicit media_source(media::media_sink& sink);

    result<void> configure_audio(audio_source_config config);
    [[nodiscard]] audio_source_config audio_config() const { return audio_config_; }
    void set_audio_iv(std::span<const std::byte, 16> iv);
    result<void> derive_audio_key(std::span<const std::byte> keymsg,
                                  std::span<const std::byte> ekey,
                                  std::span<const std::byte, 32> shared_secret);
    [[nodiscard]] bool audio_keys_ready() const { return audio_keys_ready_; }
    void reset_audio_packets();
    audio_receive_result receive_audio_rtp(std::span<const std::byte> rtp_packet,
                                           bool retransmitted);
    [[nodiscard]] audio_source_stats audio_stats() const;

    result<void> start_video(const video_source_config& config);
    result<void> receive_mirror_payload(uint8_t payload_type, uint8_t payload_flag, uint8_t option0,
                                        std::span<const std::byte> payload);
    [[nodiscard]] bool video_open() const;
    [[nodiscard]] video_source_stats video_stats() const;
    void stop_video();

private:
    struct buffered_audio_packet {
        uint16_t seqnum = 0;
        std::vector<std::byte> packet;
        bool retransmitted = false;
    };

    static media::audio_stream_config make_audio_stream_config(audio_source_config config);
    bool queue_audio_packet(std::span<const std::byte> rtp_packet, uint16_t seqnum,
                            bool retransmitted);
    void drain_audio_packets();
    void process_audio_packet(std::span<const std::byte> rtp_packet, bool retransmitted);
    result<void> configure_video_decryption(const video_source_config& config);
    result<void> process_video_frame(uint8_t payload_flag, std::span<const std::byte> payload);
    result<void> process_sps_pps(std::span<const std::byte> payload);

    media::media_sink& sink_;
    audio_source_config audio_config_;
    std::array<std::byte, 16> audio_aes_key_{};
    std::array<std::byte, 16> audio_aes_iv_{};
    bool audio_keys_ready_ = false;
    bool audio_sequence_started_ = false;
    uint16_t next_audio_seqnum_ = 0;
    bool audio_resend_pending_ = false;
    uint16_t audio_resend_pending_start_ = 0;
    audio_source_stats audio_stats_;
    std::vector<buffered_audio_packet> audio_pending_packets_;

    std::optional<crypto::aes_ctr_decryptor> video_decryptor_;
    bool video_active_ = false;
    std::vector<std::byte> sps_pps_data_;
    bool video_is_h265_ = false;
    bool sps_pps_sent_ = false;
    video_source_stats video_stats_;
};

std::string_view audio_codec_label(uint8_t codec_tag);

}  // namespace mirage::protocols::airplay
