#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/core.hpp"

namespace mirage {

struct receiver_client_stream_status {
    std::string kind;
    std::string health;
    std::string reason;
    uint64_t received_packets = 0;
    uint64_t decoded_packets = 0;
    uint64_t silent_or_marker = 0;
    uint64_t gaps = 0;
    uint64_t resend_requests = 0;
    uint64_t stale_or_redundant = 0;
    uint64_t duplicates = 0;
    uint64_t invalid = 0;
    uint64_t pending = 0;
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    uint64_t decrypted_frames = 0;
    uint64_t decrypt_failures = 0;
    uint64_t decode_failures = 0;
};

struct receiver_client_status {
    uint64_t id = 0;
    protocol protocol_id = protocol::airplay;
    std::string name;
    std::string address;
    std::string state = "connected";
    int64_t connected_at = 0;
    std::vector<receiver_client_stream_status> streams;
};

class receiver_session_observer {
public:
    virtual ~receiver_session_observer() = default;

    virtual uint64_t client_connected(receiver_client_status client) = 0;
    virtual void client_disconnected(uint64_t client_id) = 0;
    virtual void client_stream_updated(uint64_t client_id,
                                       receiver_client_stream_status stream) {
        static_cast<void>(client_id);
        static_cast<void>(stream);
    }
};

}  // namespace mirage
