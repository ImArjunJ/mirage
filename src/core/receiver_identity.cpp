#include "core/receiver_identity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

std::string trim_ascii(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                              value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t' ||
                                    value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

bool write_identity_key(const std::filesystem::path& path,
                        const std::array<std::byte, 32>& private_key,
                        std::string* error = nullptr) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        if (error != nullptr) {
            *error = std::format("identity key path is a directory: {}", path.string());
        }
        return false;
    }
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            if (error != nullptr) {
                *error = std::format("could not create identity key directory {}: {}",
                                     path.parent_path().string(), ec.message());
            }
            return false;
        }
    }

    auto encoded = base64_encode(private_key);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error != nullptr) {
            *error = std::format("could not write identity key: {}", path.string());
        }
        return false;
    }
    file << encoded << "\n";
    file.close();
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    return true;
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

result<receiver_identity_keypair> load_or_create_receiver_identity_keypair(
    const std::filesystem::path& path) {
    std::vector<std::string> warnings;
    if (std::filesystem::exists(path)) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            warnings.push_back(std::format("identity key is not a regular file: {}",
                                           path.string()));
        } else if (std::ifstream file(path, std::ios::binary); file) {
            std::string encoded((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            auto decoded = base64_decode(trim_ascii(std::move(encoded)));
            if (decoded && decoded->size() == 32) {
                std::array<std::byte, 32> private_key{};
                std::copy_n(decoded->begin(), private_key.size(), private_key.begin());
                auto keypair = crypto::ed25519_keypair::from_private_key(private_key);
                if (keypair) {
                    return receiver_identity_keypair{
                        .keypair = std::move(*keypair),
                        .source = receiver_identity_key_source::loaded,
                        .warnings = std::move(warnings),
                    };
                }
                warnings.push_back(
                    std::format("identity key could not be loaded: {}", keypair.error().message));
            } else if (decoded) {
                warnings.push_back(
                    std::format("identity key has {} bytes, expected 32: {}", decoded->size(),
                                path.string()));
            } else {
                warnings.push_back(
                    std::format("identity key is not valid base64: {}", decoded.error().message));
            }
        } else {
            warnings.push_back(std::format("could not open identity key: {}", path.string()));
        }
    }

    auto keypair = crypto::ed25519_keypair::generate();
    if (!keypair) {
        return std::unexpected(keypair.error());
    }
    auto private_key = keypair->private_key();
    if (private_key) {
        std::string error;
        if (write_identity_key(path, *private_key, &error)) {
            return receiver_identity_keypair{
                .keypair = std::move(*keypair),
                .source = receiver_identity_key_source::created,
                .warnings = std::move(warnings),
            };
        }
        if (!error.empty()) {
            warnings.push_back(std::move(error));
        }
    } else {
        warnings.push_back(
            std::format("could not persist receiver identity: {}", private_key.error().message));
    }

    return receiver_identity_keypair{
        .keypair = std::move(*keypair),
        .source = receiver_identity_key_source::transient,
        .warnings = std::move(warnings),
    };
}

}  // namespace mirage
