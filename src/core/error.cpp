#include <array>
#include <cstddef>
#include <cstdio>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "core/core.hpp"
namespace mirage {
namespace {
constexpr std::string_view base64_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";
constexpr int base64_index(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}
}  // namespace
std::string base64_encode(std::span<const std::byte> data) {
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < data.size()) {
        size_t remaining = data.size() - i;
        uint32_t octet_a = static_cast<uint8_t>(data[i++]);
        uint32_t octet_b = (i < data.size()) ? static_cast<uint8_t>(data[i++]) : 0;
        uint32_t octet_c = (i < data.size()) ? static_cast<uint8_t>(data[i++]) : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        encoded += base64_chars[(triple >> 18) & 0x3F];
        encoded += base64_chars[(triple >> 12) & 0x3F];
        encoded += (remaining < 2) ? '=' : base64_chars[(triple >> 6) & 0x3F];
        encoded += (remaining < 3) ? '=' : base64_chars[triple & 0x3F];
    }
    return encoded;
}
result<std::vector<std::byte>> base64_decode(std::string_view encoded) {
    if (encoded.size() % 4 != 0) {
        return std::unexpected(mirage_error::crypto("invalid base64 length"));
    }
    size_t output_size = (encoded.size() / 4) * 3;
    if (!encoded.empty() && encoded.back() == '=') {
        output_size--;
    }
    if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') {
        output_size--;
    }
    std::vector<std::byte> decoded;
    decoded.reserve(output_size);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        int a = base64_index(encoded[i]);
        int b = base64_index(encoded[i + 1]);
        int c = (encoded[i + 2] == '=') ? 0 : base64_index(encoded[i + 2]);
        int d = (encoded[i + 3] == '=') ? 0 : base64_index(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return std::unexpected(mirage_error::crypto("invalid base64 character"));
        }
        uint32_t triple = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
                          (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
        decoded.push_back(static_cast<std::byte>((triple >> 16) & 0xFF));
        if (encoded[i + 2] != '=') {
            decoded.push_back(static_cast<std::byte>((triple >> 8) & 0xFF));
        }
        if (encoded[i + 3] != '=') {
            decoded.push_back(static_cast<std::byte>(triple & 0xFF));
        }
    }
    return decoded;
}
std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    auto high = dist(gen);
    auto low = dist(gen);
    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    std::array<char, 37> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<uint32_t>(high >> 32), static_cast<uint16_t>((high >> 16) & 0xFFFF),
                  static_cast<uint16_t>(high & 0xFFFF), static_cast<uint16_t>(low >> 48),
                  static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFULL));
    return std::string(buffer.data());
}
}  // namespace mirage
