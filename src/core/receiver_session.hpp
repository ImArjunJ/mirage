#pragma once

#include <cstdint>
#include <string_view>

#include "core/core.hpp"
#include "core/receiver_adapter.hpp"
#include "io/io.hpp"

namespace mirage {

struct receiver_session_capabilities {
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

class receiver_session {
public:
    virtual ~receiver_session() = default;

    [[nodiscard]] virtual protocol id() const = 0;
    [[nodiscard]] virtual uint16_t port() const = 0;
    [[nodiscard]] virtual receiver_session_capabilities capabilities() const = 0;

    virtual result<void> start(receiver_adapter_registry& adapters) = 0;
    virtual io::task<void> run() = 0;
    virtual void stop(receiver_adapter_registry& adapters) = 0;
};

}  // namespace mirage
