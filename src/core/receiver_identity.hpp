#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace mirage {

struct protocol_receiver_identity {
    std::string stable_id;
    std::string uuid;
    std::string short_id;
    std::string bootstrap_id;
};

protocol_receiver_identity derive_protocol_identity(
    std::span<const std::byte, 32> public_key, std::string_view protocol_label);

}  // namespace mirage
