#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "core/receiver_client.hpp"

namespace mirage {

namespace crypto {
class ed25519_keypair;
}

namespace io {
class io_context;
}

class receiver_session;
struct receiver_source_descriptor;

struct receiver_source_runtime {
    io::io_context* io_context = nullptr;
    const crypto::ed25519_keypair* receiver_identity = nullptr;
    const std::array<std::byte, 32>* receiver_public_key = nullptr;
    receiver_session_observer* session_observer = nullptr;
    std::string_view device_name;
    std::string_view mac_address;
};

using receiver_source_validate_fn = result<void> (*)(const receiver_source_descriptor& source,
                                                     const receiver_source_runtime& runtime);
using receiver_source_session_factory = result<std::unique_ptr<receiver_session>> (*)(
    const receiver_source_descriptor& source, const receiver_source_runtime& runtime);

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
    receiver_source_validate_fn validate_source = nullptr;
    receiver_source_session_factory session_factory = nullptr;

    [[nodiscard]] result<void> validate(const receiver_source_runtime& runtime) const;
    [[nodiscard]] result<std::unique_ptr<receiver_session>> create_session(
        const receiver_source_runtime& runtime) const;
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
    uint64_t received_packets = 0;
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
    uint64_t decrypted_frames = 0;
    uint64_t decrypt_failures = 0;
    uint64_t decode_failures = 0;
};

[[nodiscard]] receiver_stream_health classify_audio_stream(
    const receiver_audio_stream_summary& summary);
[[nodiscard]] receiver_stream_health classify_video_stream(
    const receiver_video_stream_summary& summary);
[[nodiscard]] std::string_view audio_stream_health_reason(
    const receiver_audio_stream_summary& summary);
[[nodiscard]] std::string_view video_stream_health_reason(
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
