#include "core/receiver_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "crypto/crypto.hpp"

namespace mirage {
namespace {

std::string hex_bytes(std::span<const std::byte> bytes, bool uppercase = false) {
    std::string out;
    out.reserve(bytes.size() * 2);
    constexpr std::string_view lower = "0123456789abcdef";
    constexpr std::string_view upper = "0123456789ABCDEF";
    const auto alphabet = uppercase ? upper : lower;
    for (auto b : bytes) {
        const auto value = static_cast<uint8_t>(b);
        out.push_back(alphabet[value >> 4]);
        out.push_back(alphabet[value & 0x0F]);
    }
    return out;
}

std::array<std::byte, 64> identity_hash(std::span<const std::byte, 32> public_key,
                                        std::string_view protocol_label) {
    std::vector<std::byte> seed;
    seed.reserve(protocol_label.size() + 1 + public_key.size());
    for (char c : protocol_label) {
        seed.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    seed.push_back(std::byte{0});
    seed.insert(seed.end(), public_key.begin(), public_key.end());
    return crypto::sha512(seed);
}

std::string uuid_from_hash(std::span<const std::byte, 16> bytes) {
    std::array<std::byte, 16> id{};
    std::copy(bytes.begin(), bytes.end(), id.begin());
    id[6] = static_cast<std::byte>((static_cast<uint8_t>(id[6]) & 0x0F) | 0x80);
    id[8] = static_cast<std::byte>((static_cast<uint8_t>(id[8]) & 0x3F) | 0x80);
    return std::format("{}-{}-{}-{}-{}", hex_bytes(std::span{id}.subspan(0, 4)),
                       hex_bytes(std::span{id}.subspan(4, 2)),
                       hex_bytes(std::span{id}.subspan(6, 2)),
                       hex_bytes(std::span{id}.subspan(8, 2)),
                       hex_bytes(std::span{id}.subspan(10, 6)));
}

}  // namespace

protocol_receiver_identity derive_protocol_identity(
    std::span<const std::byte, 32> public_key, std::string_view protocol_label) {
    auto hash = identity_hash(public_key, protocol_label);
    std::array<std::byte, 16> uuid_bytes{};
    std::copy_n(hash.begin(), uuid_bytes.size(), uuid_bytes.begin());
    return {
        .stable_id = hex_bytes(std::span{hash}.subspan(0, 16)),
        .uuid = uuid_from_hash(uuid_bytes),
        .short_id = hex_bytes(std::span{hash}.subspan(16, 4), true),
        .bootstrap_id = hex_bytes(std::span{hash}.subspan(20, 6), true),
    };
}

}  // namespace mirage
