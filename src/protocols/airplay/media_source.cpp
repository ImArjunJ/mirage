#include "protocols/airplay/media_source.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.hpp"
#include "media/media.hpp"
#include "protocols/airplay_protocol.hpp"

namespace mirage::protocols::airplay {
namespace {

bool looks_like_aac_eld(uint8_t first_byte) {
    return first_byte == 0x8c || first_byte == 0x8d || first_byte == 0x8e || first_byte == 0x80 ||
           first_byte == 0x81 || first_byte == 0x82;
}

bool looks_like_alac(uint8_t first_byte) {
    return first_byte == 0x20;
}

std::array<std::byte, 16> fairplay_audio_key(std::span<const std::byte, 164> keymsg,
                                             std::span<const std::byte, 72> ekey,
                                             std::span<const std::byte> shared_secret) {
    auto aeskey = crypto::fairplay_decrypt_key(keymsg, ekey);
    std::array<std::byte, 48> combined{};
    std::copy_n(aeskey.begin(), 16, combined.begin());
    std::copy_n(shared_secret.begin(), 32, combined.begin() + 16);
    auto hashed_key = crypto::sha512(combined);
    std::copy_n(hashed_key.begin(), 16, aeskey.begin());
    return aeskey;
}

std::optional<uint32_t> read_nal_length(std::span<const std::byte> data, size_t offset) {
    if (offset + 4 > data.size()) {
        return std::nullopt;
    }
    return (static_cast<uint32_t>(data[offset]) << 24) |
           (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
}

}  // namespace

media_source::media_source(media::media_sink& sink) : sink_(sink) {}

std::string_view audio_codec_label(uint8_t codec_tag) {
    return codec_tag == 2 ? "ALAC" : "AAC-ELD";
}

media::audio_stream_config media_source::make_audio_stream_config(audio_source_config config) {
    media::audio_stream_config stream_config{
        .codec = config.codec_tag == 2 ? audio_codec::alac : audio_codec::aac,
        .sample_rate = config.sample_rate,
        .channels = config.channels,
        .frames_per_packet = config.frames_per_packet,
        .codec_tag = config.codec_tag,
        .codec_config = {},
    };

    if (config.codec_tag == 2) {
        uint32_t alac_spf = static_cast<uint32_t>(config.frames_per_packet);
        uint32_t alac_sr = static_cast<uint32_t>(config.sample_rate);
        stream_config.codec_config.resize(36);
        stream_config.codec_config[0] = std::byte{0x00};
        stream_config.codec_config[1] = std::byte{0x00};
        stream_config.codec_config[2] = std::byte{0x00};
        stream_config.codec_config[3] = std::byte{0x24};
        stream_config.codec_config[4] = std::byte{'a'};
        stream_config.codec_config[5] = std::byte{'l'};
        stream_config.codec_config[6] = std::byte{'a'};
        stream_config.codec_config[7] = std::byte{'c'};
        stream_config.codec_config[8] = std::byte{0x00};
        stream_config.codec_config[9] = std::byte{0x00};
        stream_config.codec_config[10] = std::byte{0x00};
        stream_config.codec_config[11] = std::byte{0x00};
        stream_config.codec_config[12] = std::byte{static_cast<uint8_t>((alac_spf >> 24) & 0xFF)};
        stream_config.codec_config[13] = std::byte{static_cast<uint8_t>((alac_spf >> 16) & 0xFF)};
        stream_config.codec_config[14] = std::byte{static_cast<uint8_t>((alac_spf >> 8) & 0xFF)};
        stream_config.codec_config[15] = std::byte{static_cast<uint8_t>(alac_spf & 0xFF)};
        stream_config.codec_config[16] = std::byte{0x00};
        stream_config.codec_config[17] = std::byte{0x10};
        stream_config.codec_config[18] = std::byte{0x28};
        stream_config.codec_config[19] = std::byte{0x0A};
        stream_config.codec_config[20] = std::byte{0x0E};
        stream_config.codec_config[21] = std::byte{0x02};
        stream_config.codec_config[22] = std::byte{0x00};
        stream_config.codec_config[23] = std::byte{0xFF};
        stream_config.codec_config[24] = std::byte{0x00};
        stream_config.codec_config[25] = std::byte{0x00};
        stream_config.codec_config[26] = std::byte{0x00};
        stream_config.codec_config[27] = std::byte{0x00};
        stream_config.codec_config[28] = std::byte{0x00};
        stream_config.codec_config[29] = std::byte{0x00};
        stream_config.codec_config[30] = std::byte{0x00};
        stream_config.codec_config[31] = std::byte{0x00};
        stream_config.codec_config[32] = std::byte{static_cast<uint8_t>((alac_sr >> 24) & 0xFF)};
        stream_config.codec_config[33] = std::byte{static_cast<uint8_t>((alac_sr >> 16) & 0xFF)};
        stream_config.codec_config[34] = std::byte{static_cast<uint8_t>((alac_sr >> 8) & 0xFF)};
        stream_config.codec_config[35] = std::byte{static_cast<uint8_t>(alac_sr & 0xFF)};
    }
    return stream_config;
}

result<void> media_source::configure_audio(audio_source_config config) {
    auto configured = sink_.on_audio_setup(make_audio_stream_config(config));
    if (!configured) {
        return configured;
    }
    audio_config_ = config;
    return {};
}

void media_source::set_audio_iv(std::span<const std::byte, 16> iv) {
    std::copy_n(iv.begin(), 16, audio_aes_iv_.begin());
}

result<void> media_source::derive_audio_key(std::span<const std::byte> keymsg,
                                            std::span<const std::byte> ekey,
                                            std::span<const std::byte, 32> shared_secret) {
    if (keymsg.size() != 164 || ekey.size() != 72) {
        return std::unexpected(mirage_error::crypto(
            std::format("invalid FairPlay key material: keymsg={} bytes, ekey={} bytes",
                        keymsg.size(), ekey.size())));
    }
    std::array<std::byte, 164> keymsg_arr{};
    std::array<std::byte, 72> ekey_arr{};
    std::copy_n(keymsg.begin(), 164, keymsg_arr.begin());
    std::copy_n(ekey.begin(), 72, ekey_arr.begin());
    audio_aes_key_ = fairplay_audio_key(keymsg_arr, ekey_arr, shared_secret);
    audio_keys_ready_ = true;
    log::info("Audio AES-CBC key derived (with ECDH hashing)");
    return {};
}

void media_source::reset_audio_packets() {
    audio_sequence_started_ = false;
    next_audio_seqnum_ = 0;
    audio_resend_pending_ = false;
    audio_resend_pending_start_ = 0;
    audio_stats_ = {};
    audio_pending_packets_.clear();
}

audio_source_stats media_source::audio_stats() const {
    auto stats = audio_stats_;
    stats.pending = audio_pending_packets_.size();
    return stats;
}

audio_receive_result media_source::receive_audio_rtp(std::span<const std::byte> rtp_packet,
                                                     bool retransmitted) {
    audio_receive_result receive_result{};
    if (rtp_packet.size() <= media::rtp_header::size) {
        return receive_result;
    }
    ++audio_stats_.received_packets;

    auto seqnum =
        static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(rtp_packet[2])) << 8) |
                              static_cast<uint16_t>(static_cast<uint8_t>(rtp_packet[3])));

    if (!audio_sequence_started_) {
        audio_sequence_started_ = true;
        next_audio_seqnum_ = seqnum;
    } else if (!retransmitted) {
        int gap = audio_seq_delta(seqnum, next_audio_seqnum_);
        if (gap > 0 && gap < 1000) {
            if (!audio_resend_pending_ || audio_resend_pending_start_ != next_audio_seqnum_) {
                ++audio_stats_.gaps;
                log::debug("Audio gap detected: expected seq {}, got {}, missing {} packet(s)",
                           next_audio_seqnum_, seqnum, gap);
                auto count = clamp_resend_count(gap);
                if (count != 0) {
                    ++audio_stats_.resend_requests;
                    audio_resend_pending_ = true;
                    audio_resend_pending_start_ = next_audio_seqnum_;
                    receive_result.resend = audio_resend_request{
                        .start_seqnum = next_audio_seqnum_,
                        .count = count,
                    };
                }
            }
        }
    }

    receive_result.accepted = queue_audio_packet(rtp_packet, seqnum, retransmitted);
    if (receive_result.accepted) {
        drain_audio_packets();
    }
    return receive_result;
}

bool media_source::queue_audio_packet(std::span<const std::byte> rtp_packet, uint16_t seqnum,
                                      bool retransmitted) {
    int delta = audio_seq_delta(seqnum, next_audio_seqnum_);
    if (delta < 0) {
        ++audio_stats_.stale_or_redundant;
        if (audio_stats_.stale_or_redundant <= 5 || audio_stats_.stale_or_redundant % 1000 == 0) {
            log::trace("Dropping stale/redundant audio packet seq={} next={}", seqnum,
                       next_audio_seqnum_);
        }
        return false;
    }

    auto duplicate =
        std::ranges::find(audio_pending_packets_, seqnum, &buffered_audio_packet::seqnum);
    if (duplicate != audio_pending_packets_.end()) {
        ++audio_stats_.duplicates;
        if (audio_stats_.duplicates <= 5 || audio_stats_.duplicates % 500 == 0) {
            log::trace("Dropping duplicate audio packet seq={}", seqnum);
        }
        return false;
    }

    audio_pending_packets_.push_back(buffered_audio_packet{
        .seqnum = seqnum,
        .packet = std::vector<std::byte>(rtp_packet.begin(), rtp_packet.end()),
        .retransmitted = retransmitted,
    });

    if (audio_pending_packets_.size() > max_pending_audio_packets) {
        auto next_it = std::ranges::min_element(
            audio_pending_packets_, [this](const auto& lhs, const auto& rhs) {
                return audio_seq_delta(lhs.seqnum, next_audio_seqnum_) <
                       audio_seq_delta(rhs.seqnum, next_audio_seqnum_);
            });
        if (next_it != audio_pending_packets_.end()) {
            int skipped = audio_seq_delta(next_it->seqnum, next_audio_seqnum_);
            if (skipped > 0) {
                log::warn("Skipping {} missing audio packet(s) after resend window expired",
                          skipped);
                next_audio_seqnum_ = next_it->seqnum;
            }
        }
    }

    return true;
}

void media_source::drain_audio_packets() {
    while (audio_sequence_started_) {
        auto it = std::ranges::find(audio_pending_packets_, next_audio_seqnum_,
                                    &buffered_audio_packet::seqnum);
        if (it == audio_pending_packets_.end()) {
            return;
        }

        auto packet = std::move(it->packet);
        auto retransmitted = it->retransmitted;
        audio_pending_packets_.erase(it);
        if (retransmitted) {
            log::trace("Recovered audio packet seq={}", next_audio_seqnum_);
        }
        process_audio_packet(std::span<const std::byte>(packet.data(), packet.size()),
                             retransmitted);
        next_audio_seqnum_ = static_cast<uint16_t>(next_audio_seqnum_ + 1);
        if (audio_resend_pending_ &&
            audio_seq_delta(next_audio_seqnum_, audio_resend_pending_start_) > 0) {
            audio_resend_pending_ = false;
        }
    }
}

void media_source::process_audio_packet(std::span<const std::byte> rtp_packet, bool retransmitted) {
    auto payload_start = rtp_packet.data() + media::rtp_header::size;
    auto payload_len = rtp_packet.size() - media::rtp_header::size;

    if (payload_len == 0) {
        ++audio_stats_.silent_or_marker;
        return;
    }

    constexpr std::array<std::byte, 4> no_data_marker{std::byte{0x00}, std::byte{0x68},
                                                      std::byte{0x34}, std::byte{0x00}};
    if (payload_len == 4 && std::equal(payload_start, payload_start + 4, no_data_marker.begin())) {
        ++audio_stats_.silent_or_marker;
        return;
    }

    if (audio_config_.codec_tag == 2 && payload_len == 44) {
        ++audio_stats_.silent_or_marker;
        return;
    }

    std::vector<std::byte> decrypted(payload_len);
    if (audio_keys_ready_ && payload_len > 0) {
        auto encrypted_len = (payload_len / 16) * 16;
        if (encrypted_len > 0) {
            crypto::aes_cbc_decrypt(audio_aes_key_, audio_aes_iv_,
                                    std::span<const std::byte>(payload_start, encrypted_len),
                                    std::span<std::byte>(decrypted.data(), encrypted_len));
        }
        if (payload_len > encrypted_len) {
            std::copy_n(payload_start + encrypted_len, payload_len - encrypted_len,
                        decrypted.data() + encrypted_len);
        }
    } else {
        std::copy_n(payload_start, payload_len, decrypted.data());
    }

    auto first_payload_byte = static_cast<uint8_t>(decrypted[0]);
    if (audio_config_.codec_tag == 2 && !looks_like_alac(first_payload_byte)) {
        if (looks_like_aac_eld(first_payload_byte)) {
            log::warn("Audio payload looks like AAC-ELD while configured as ALAC; switching");
            auto aac_config = audio_config_;
            aac_config.codec_tag = 8;
            aac_config.frames_per_packet = 480;
            if (auto configured = configure_audio(aac_config); !configured) {
                log::error("Failed to switch audio decoder to AAC-ELD: {}",
                           configured.error().message);
                return;
            }
        } else {
            ++audio_stats_.invalid;
            if (audio_stats_.invalid <= 5 || audio_stats_.invalid % 500 == 0) {
                log::warn("Skipping invalid ALAC payload: first byte=0x{:02x}, len={}",
                          first_payload_byte, decrypted.size());
            }
            return;
        }
    } else if (audio_config_.codec_tag == 8 && !looks_like_aac_eld(first_payload_byte)) {
        ++audio_stats_.invalid;
        if (audio_stats_.invalid <= 5 || audio_stats_.invalid % 500 == 0) {
            if (looks_like_alac(first_payload_byte)) {
                log::warn("Audio payload looks like ALAC while configured as AAC-ELD; skipping");
            } else {
                log::warn("Skipping invalid AAC-ELD payload: first byte=0x{:02x}, len={}",
                          first_payload_byte, decrypted.size());
            }
        }
        return;
    }

    uint16_t sequence =
        static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(rtp_packet[2])) << 8) |
                              static_cast<uint16_t>(static_cast<uint8_t>(rtp_packet[3])));
    uint32_t timestamp = (static_cast<uint32_t>(static_cast<uint8_t>(rtp_packet[4])) << 24) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(rtp_packet[5])) << 16) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(rtp_packet[6])) << 8) |
                         static_cast<uint32_t>(static_cast<uint8_t>(rtp_packet[7]));
    media::media_packet packet{
        .stream = media::media_stream_type::audio,
        .payload = std::move(decrypted),
        .sequence = sequence,
        .timestamp = timestamp,
        .keyframe = false,
        .retransmitted = retransmitted,
    };
    if (auto delivered = sink_.on_audio_packet(packet); !delivered) {
        ++audio_stats_.invalid;
        if (audio_stats_.invalid <= 5 || audio_stats_.invalid % 500 == 0) {
            log::warn("Audio packet delivery failed: {}", delivered.error().message);
        }
        return;
    }

    ++audio_stats_.decoded_packets;
    if (audio_stats_.decoded_packets == 1 || audio_stats_.decoded_packets % 500 == 0) {
        log::info("Audio: {} packets decoded", audio_stats_.decoded_packets);
    }
}

result<void> media_source::start_video(const video_source_config& config) {
    video_stats_ = {};
    sps_pps_data_.clear();
    video_is_h265_ = false;
    sps_pps_sent_ = false;
    auto video_setup = sink_.on_video_setup(config.sink_config);
    if (!video_setup) {
        video_active_ = false;
        return video_setup;
    }
    video_active_ = true;
    auto decrypt_configured = configure_video_decryption(config);
    if (!decrypt_configured) {
        log::warn("{}", decrypt_configured.error().message);
    }
    return {};
}

result<void> media_source::configure_video_decryption(const video_source_config& config) {
    video_decryptor_.reset();
    if (config.stream_connection_id == 0) {
        return std::unexpected(mirage_error::crypto(std::format(
            "Cannot initialize video decryption: streamID={}", config.stream_connection_id)));
    }
    if (config.keymsg.size() != 164 || config.ekey.size() != 72 ||
        config.shared_secret.size() != 32) {
        return std::unexpected(mirage_error::crypto(std::format(
            "Cannot initialize video decryption: keymsg={} bytes, ekey={} bytes, secret={} bytes",
            config.keymsg.size(), config.ekey.size(), config.shared_secret.size())));
    }

    log::debug("FairPlay key material present: keymsg={} bytes, ekey={} bytes",
               config.keymsg.size(), config.ekey.size());
    std::array<std::byte, 164> keymsg_arr{};
    std::array<std::byte, 72> ekey_arr{};
    std::copy_n(config.keymsg.begin(), 164, keymsg_arr.begin());
    std::copy_n(config.ekey.begin(), 72, ekey_arr.begin());
    auto aeskey = fairplay_audio_key(keymsg_arr, ekey_arr, config.shared_secret);
    auto key_hash = crypto::sha512_concat("AirPlayStreamKey", config.stream_connection_id, aeskey);
    auto iv_hash = crypto::sha512_concat("AirPlayStreamIV", config.stream_connection_id, aeskey);
    std::array<std::byte, 16> video_key{};
    std::array<std::byte, 16> video_iv{};
    std::copy_n(key_hash.begin(), 16, video_key.begin());
    std::copy_n(iv_hash.begin(), 16, video_iv.begin());
    log::debug("Derived video decryption material for streamConnectionID={}",
               config.stream_connection_id);
    auto decryptor = crypto::aes_ctr_decryptor::create(video_key, video_iv);
    if (!decryptor) {
        return std::unexpected(mirage_error::crypto(
            std::format("Failed to initialize video decryption: {}", decryptor.error().message)));
    }
    video_decryptor_ = std::move(*decryptor);
    audio_aes_key_ = aeskey;
    audio_keys_ready_ = true;
    log::info("Video decryption initialized for streamConnectionID={}",
              config.stream_connection_id);
    log::info("Audio AES-CBC key ready");
    return {};
}

result<void> media_source::receive_mirror_payload(uint8_t payload_type, uint8_t payload_flag,
                                                  uint8_t option0,
                                                  std::span<const std::byte> payload) {
    if (option0 == 0x1e || option0 == 0x5e) {
        video_is_h265_ = true;
    }
    switch (payload_type) {
        case 0x00:
            return process_video_frame(payload_flag, payload);
        case 0x01:
            return process_sps_pps(payload);
        case 0x02:
            log::trace("Mirror heartbeat packet");
            return {};
        case 0x05:
            log::trace("Streaming report ({} bytes)", payload.size());
            return {};
        default:
            log::debug("Unknown mirror packet type 0x{:02x} 0x{:02x}, {} bytes", payload_type,
                       payload_flag, payload.size());
            return {};
    }
}

result<void> media_source::process_video_frame(uint8_t payload_flag,
                                               std::span<const std::byte> payload) {
    bool is_keyframe = (payload_flag == 0x10);
    ++video_stats_.frames;
    if (is_keyframe) {
        ++video_stats_.keyframes;
    }
    if (video_stats_.frames % 60 == 1) {
        log::info("Video: {} frames ({} keyframes), {} codec, payload {} bytes{}",
                  video_stats_.frames, video_stats_.keyframes, video_is_h265_ ? "H.265" : "H.264",
                  payload.size(), is_keyframe ? " [KEYFRAME]" : "");
    }
    if (!video_decryptor_ || payload.empty()) {
        return {};
    }

    if (video_stats_.frames <= 3) {
        log::trace("Frame {} decrypting {} bytes", video_stats_.frames, payload.size());
    }
    std::vector<std::byte> decrypted(payload.size());
    auto result = video_decryptor_->decrypt(payload, decrypted);
    if (!result) {
        ++video_stats_.decrypt_failures;
        return std::unexpected(result.error());
    }
    ++video_stats_.decrypted_frames;
    if (video_stats_.frames <= 3) {
        log::trace("Frame {} decrypted {} bytes", video_stats_.frames, payload.size());
    }

    auto decrypted_span = std::span<const std::byte>(decrypted.data(), decrypted.size());
    size_t offset = 0;
    int nal_count = 0;
    bool valid = true;
    while (offset + 4 < decrypted.size() && valid) {
        auto nal_len = read_nal_length(decrypted_span, offset).value_or(0);
        if (nal_len == 0 || offset + 4 + nal_len > decrypted.size()) {
            valid = false;
            break;
        }
        uint8_t nal_header = static_cast<uint8_t>(decrypted[offset + 4]);
        if (nal_header & 0x80) {
            valid = false;
            break;
        }
        (void)(video_is_h265_ ? ((nal_header >> 1) & 0x3F) : (nal_header & 0x1F));
        ++nal_count;
        offset += 4 + nal_len;
    }
    if (video_stats_.frames <= 5 || (video_stats_.frames % 300 == 1)) {
        if (valid && offset == decrypted.size()) {
            log::info("Frame {} decrypted successfully: {} NAL unit(s)", video_stats_.frames,
                      nal_count);
        } else {
            log::warn("Frame {} decryption check failed at offset {}/{}", video_stats_.frames,
                      offset, decrypted.size());
        }
    }
    if (!sink_.video_open()) {
        return {};
    }

    std::vector<std::byte> annex_b;
    constexpr std::array<std::byte, 4> start_code{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                                  std::byte{0x01}};
    if (is_keyframe && !sps_pps_sent_ && !sps_pps_data_.empty()) {
        annex_b.insert(annex_b.end(), sps_pps_data_.begin(), sps_pps_data_.end());
        sps_pps_sent_ = true;
        log::info("Prepended SPS/PPS ({} bytes) to keyframe", sps_pps_data_.size());
    }

    size_t nal_offset = 0;
    while (nal_offset + 4 < decrypted.size()) {
        auto nal_len = read_nal_length(decrypted_span, nal_offset).value_or(0);
        if (nal_len == 0 || nal_offset + 4 + nal_len > decrypted.size()) {
            break;
        }
        annex_b.insert(annex_b.end(), start_code.begin(), start_code.end());
        annex_b.insert(annex_b.end(),
                       decrypted.begin() + static_cast<std::ptrdiff_t>(nal_offset + 4),
                       decrypted.begin() + static_cast<std::ptrdiff_t>(nal_offset + 4 + nal_len));
        nal_offset += 4 + nal_len;
    }

    media::media_packet packet{
        .stream = media::media_stream_type::video,
        .payload = std::move(annex_b),
        .sequence = video_stats_.frames,
        .timestamp = 0,
        .keyframe = is_keyframe,
        .retransmitted = false,
    };
    auto decode_result = sink_.on_video_packet(packet);
    if (!decode_result) {
        ++video_stats_.decode_failures;
        if (video_stats_.decode_failures <= 5 || video_stats_.frames % 60 == 1) {
            log::debug("Decode: {}", decode_result.error().message);
        }
    }
    return {};
}

result<void> media_source::process_sps_pps(std::span<const std::byte> payload) {
    if (payload.size() < 11) {
        log::warn("SPS/PPS packet too small: {} bytes", payload.size());
        return {};
    }
    auto sps_size = static_cast<size_t>((static_cast<uint16_t>(payload[6]) << 8) |
                                        static_cast<uint16_t>(payload[7]));
    if (8 + sps_size + 3 > payload.size()) {
        log::warn("SPS/PPS: invalid sps_size {} for payload {}", sps_size, payload.size());
        return {};
    }
    auto pps_size = static_cast<size_t>((static_cast<uint16_t>(payload[8 + sps_size + 1]) << 8) |
                                        static_cast<uint16_t>(payload[8 + sps_size + 2]));
    if (8 + sps_size + 3 + pps_size > payload.size()) {
        log::warn("SPS/PPS: invalid pps_size {} for payload {}", pps_size, payload.size());
        return {};
    }
    log::info("Received SPS ({} bytes) + PPS ({} bytes), {} codec", sps_size, pps_size,
              video_is_h265_ ? "H.265" : "H.264");
    constexpr std::array<std::byte, 4> start_code{std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                                                  std::byte{0x01}};
    sps_pps_data_.clear();
    sps_pps_data_.reserve(sps_size + pps_size + 8);
    sps_pps_data_.insert(sps_pps_data_.end(), start_code.begin(), start_code.end());
    sps_pps_data_.insert(sps_pps_data_.end(), payload.begin() + 8,
                         payload.begin() + static_cast<std::ptrdiff_t>(8 + sps_size));
    sps_pps_data_.insert(sps_pps_data_.end(), start_code.begin(), start_code.end());
    sps_pps_data_.insert(
        sps_pps_data_.end(), payload.begin() + static_cast<std::ptrdiff_t>(8 + sps_size + 3),
        payload.begin() + static_cast<std::ptrdiff_t>(8 + sps_size + 3 + pps_size));
    sps_pps_sent_ = false;
    log::debug("Parsed SPS/PPS Annex-B: {} bytes", sps_pps_data_.size());
    return {};
}

bool media_source::video_open() const {
    return video_active_ && sink_.video_open();
}

video_source_stats media_source::video_stats() const {
    return video_stats_;
}

void media_source::stop_video() {
    if (video_active_) {
        sink_.on_video_teardown();
    }
    video_active_ = false;
    video_decryptor_.reset();
    sps_pps_data_.clear();
    sps_pps_sent_ = false;
}

}  // namespace mirage::protocols::airplay
