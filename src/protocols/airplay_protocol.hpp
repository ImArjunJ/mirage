#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace mirage::protocols::airplay {

inline constexpr std::string_view compatibility_model = "AppleTV6,2";
inline constexpr std::string_view source_version = "220.68";
inline constexpr std::string_view info_source_version = "770.8.1";
inline constexpr uint32_t feature_low = 0x5A7FFEE6U;
inline constexpr uint32_t feature_high = 0x0U;
inline constexpr uint32_t status_flags = 0x44U;
inline constexpr int protocol_version = 2;
inline constexpr int default_sample_rate = 44100;
inline constexpr int default_channels = 2;
inline constexpr int default_audio_latency_micros = 250000;
inline constexpr uint16_t max_audio_resend_packets = 128;
inline constexpr size_t max_pending_audio_packets = 128;
inline constexpr float initial_volume_db = -20.0F;

inline std::string feature_txt() {
    return "0x5A7FFEE6,0x0";
}

inline int audio_latency_samples(int sample_rate) {
    if (sample_rate <= 0) {
        sample_rate = default_sample_rate;
    }
    double samples = static_cast<double>(sample_rate) *
                     (static_cast<double>(default_audio_latency_micros) / 1000000.0);
    return static_cast<int>(std::lround(samples));
}

inline int audio_seq_delta(uint16_t seqnum, uint16_t expected) {
    return static_cast<int16_t>(static_cast<uint16_t>(seqnum - expected));
}

inline uint16_t clamp_resend_count(int missing_packets) {
    if (missing_packets <= 0) {
        return 0;
    }
    return static_cast<uint16_t>(
        std::min(missing_packets, static_cast<int>(max_audio_resend_packets)));
}

inline float db_to_linear(float db) {
    return (db <= -30.0F) ? 0.0F : std::pow(10.0F, db / 20.0F);
}

}  // namespace mirage::protocols::airplay
