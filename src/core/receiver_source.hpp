#pragma once

#include <cstdint>
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
