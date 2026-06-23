#pragma once

#include <cstdint>
#include <string>

#include "core/core.hpp"

namespace mirage {

struct receiver_client_status {
    uint64_t id = 0;
    protocol protocol_id = protocol::airplay;
    std::string name;
    std::string address;
    std::string state = "connected";
    int64_t connected_at = 0;
};

class receiver_session_observer {
public:
    virtual ~receiver_session_observer() = default;

    virtual uint64_t client_connected(receiver_client_status client) = 0;
    virtual void client_disconnected(uint64_t client_id) = 0;
};

}  // namespace mirage
