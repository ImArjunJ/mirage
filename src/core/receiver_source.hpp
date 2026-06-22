#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"

namespace mirage {

struct receiver_source_capabilities {
    bool network_listener = false;
    bool discovery = false;
    bool pairing = false;
    bool media_setup = false;
    bool audio = false;
    bool video = false;
    bool remote_control = false;
    bool metadata = false;
    std::string_view transport;
};

struct receiver_source_descriptor {
    protocol id;
    uint16_t port = 0;
    bool enabled = false;
    bool experimental = false;
    std::string_view detail;
    receiver_source_capabilities capabilities;
};

enum class receiver_stream_health : uint8_t { clean, attention };

constexpr std::string_view to_string(receiver_stream_health health) {
    switch (health) {
        case receiver_stream_health::clean:
            return "clean";
        case receiver_stream_health::attention:
            return "attention";
    }
    std::unreachable();
}

struct receiver_audio_stream_setup {
    std::string_view codec;
    int sample_rate = 0;
    int channels = 0;
    int frames_per_packet = 0;
    uint16_t data_port = 0;
    uint16_t control_port = 0;
    std::optional<uint16_t> timing_port;
};

struct receiver_audio_stream_summary {
    uint64_t decoded_packets = 0;
    uint64_t silent_or_marker = 0;
    uint64_t gaps = 0;
    uint64_t resend_requests = 0;
    uint64_t stale_or_redundant = 0;
    uint64_t duplicates = 0;
    uint64_t invalid = 0;
    size_t pending = 0;
};

struct receiver_video_stream_summary {
    uint64_t frames = 0;
    uint64_t keyframes = 0;
};

[[nodiscard]] receiver_stream_health classify_audio_stream(
    const receiver_audio_stream_summary& summary);
[[nodiscard]] receiver_stream_health classify_video_stream(
    const receiver_video_stream_summary& summary);

void log_receiver_audio_setup(const receiver_source_descriptor& source,
                              const receiver_audio_stream_setup& setup,
                              std::string_view label = "Audio stream setup");
void log_receiver_audio_summary(const receiver_source_descriptor& source,
                                const receiver_audio_stream_summary& summary);
void log_receiver_video_summary(const receiver_source_descriptor& source,
                                const receiver_video_stream_summary& summary);

class receiver_source_registry {
public:
    explicit receiver_source_registry(std::vector<receiver_source_descriptor> sources);

    [[nodiscard]] std::span<const receiver_source_descriptor> all() const;
    [[nodiscard]] const receiver_source_descriptor* find(protocol id) const;
    [[nodiscard]] std::vector<receiver_source_descriptor> enabled() const;

private:
    std::vector<receiver_source_descriptor> sources_;
};

}  // namespace mirage
