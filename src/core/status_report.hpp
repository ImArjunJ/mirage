#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

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
    int64_t started = 0;
    std::span<const receiver_adapter_status> adapters;
    std::span<const receiver_source_descriptor> sources;
};

[[nodiscard]] std::string render_status_json(const receiver_status_report& report);

}  // namespace mirage
