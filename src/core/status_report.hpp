#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/receiver_client.hpp"
#include "core/receiver_source.hpp"

namespace mirage {

struct receiver_status_report {
    int pid = 0;
    std::string_view name;
    std::string_view ip;
    std::string_view interface_name;
    std::string_view identity_key;
    uint16_t airplay_port = 0;
    uint16_t cast_port = 0;
    uint16_t miracast_port = 0;
    int64_t started = 0;
    std::span<const receiver_adapter_status> adapters;
    std::span<const receiver_source_descriptor> sources;
    std::span<const receiver_client_status> clients;
};

struct receiver_status_media_summary {
    bool active = false;
    std::string title;
    std::string artist;
    std::string album;
    std::string artwork_type;
    int64_t artwork_bytes = 0;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
};

struct receiver_status_stream_summary {
    std::string kind;
    std::string health;
    std::string reason;
    std::optional<int64_t> received_packets;
    std::optional<int64_t> decoded_packets;
    std::optional<int64_t> frames;
    std::optional<int64_t> keyframes;
};

struct receiver_status_client_summary {
    std::string protocol;
    std::string address;
    std::string state;
    std::string name;
    receiver_status_media_summary media;
    std::vector<receiver_status_stream_summary> streams;
};

struct receiver_status_protocol_summary {
    std::string id;
    std::string state;
    std::string detail;
    std::string transport;
    std::string capabilities;
    int64_t port = 0;
    bool advertised = false;
};

struct receiver_status_summary {
    std::string name;
    std::string ip;
    std::string interface_name;
    std::string identity_key;
    std::optional<int64_t> airplay_port;
    std::optional<int64_t> cast_port;
    std::optional<int64_t> miracast_port;
    std::optional<int64_t> started;
    std::vector<receiver_status_protocol_summary> protocols;
    std::vector<receiver_status_client_summary> clients;
};

[[nodiscard]] std::string render_status_json(const receiver_status_report& report);
[[nodiscard]] receiver_status_summary parse_status_summary(std::string_view json);
[[nodiscard]] std::string render_status_summary_text(
    const receiver_status_summary& summary, int pid, bool verbose,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

}  // namespace mirage
