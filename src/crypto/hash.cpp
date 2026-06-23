#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "crypto/crypto.hpp"

namespace mirage::crypto {

std::array<std::byte, 64> sha512(std::span<const std::byte> data) {
    std::array<std::byte, 64> hash_result{};
    SHA512(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
           reinterpret_cast<unsigned char*>(hash_result.data()));
    return hash_result;
}

std::array<std::byte, 64> sha512_concat(std::string_view prefix, uint64_t stream_id,
                                        std::span<const std::byte> key) {
    char stream_id_buf[32];
    int id_len = std::snprintf(stream_id_buf, sizeof(stream_id_buf), "%" PRIu64, stream_id);
    std::array<std::byte, 64> hash_result{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha512(), nullptr);
    EVP_DigestUpdate(ctx, prefix.data(), prefix.size());
    EVP_DigestUpdate(ctx, stream_id_buf, static_cast<size_t>(id_len));
    EVP_DigestUpdate(ctx, key.data(), key.size());
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(ctx, reinterpret_cast<unsigned char*>(hash_result.data()), &hash_len);
    EVP_MD_CTX_free(ctx);
    return hash_result;
}

}  // namespace mirage::crypto
