#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "crypto/crypto.hpp"

namespace mirage {

struct protocol_receiver_identity {
    std::string stable_id;
    std::string uuid;
    std::string short_id;
    std::string bootstrap_id;
};

enum class receiver_identity_key_source { loaded, created, transient };

struct receiver_identity_keypair {
    crypto::ed25519_keypair keypair;
    receiver_identity_key_source source = receiver_identity_key_source::transient;
    std::vector<std::string> warnings;
};

protocol_receiver_identity derive_protocol_identity(
    std::span<const std::byte, 32> public_key, std::string_view protocol_label);

result<receiver_identity_keypair> load_or_create_receiver_identity_keypair(
    const std::filesystem::path& path);

}  // namespace mirage
