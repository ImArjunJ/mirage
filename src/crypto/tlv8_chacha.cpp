#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <openssl/evp.h>

#include "core/core.hpp"
#include "crypto/crypto.hpp"
namespace mirage::crypto {
namespace tlv8 {
result<std::vector<item>> parse(std::span<const std::byte> data) {
    std::vector<item> items;
    size_t offset = 0;
    while (offset < data.size()) {
        if (offset + 2 > data.size()) {
            return std::unexpected(mirage_error::crypto("TLV8: truncated header"));
        }
        auto tag = static_cast<type>(data[offset]);
        auto length = static_cast<uint8_t>(data[offset + 1]);
        offset += 2;
        if (offset + length > data.size()) {
            return std::unexpected(mirage_error::crypto("TLV8: truncated value"));
        }
        std::vector<std::byte> value(data.begin() + static_cast<ptrdiff_t>(offset),
                                     data.begin() + static_cast<ptrdiff_t>(offset + length));
        offset += length;
        if (!items.empty() && items.back().tag == tag && length == 255) {
            items.back().value.insert(items.back().value.end(), value.begin(), value.end());
        } else if (!items.empty() && items.back().tag == tag) {
            items.back().value.insert(items.back().value.end(), value.begin(), value.end());
        } else {
            items.push_back({tag, std::move(value)});
        }
    }
    return items;
}
std::vector<std::byte> encode(std::span<const item> items) {
    std::vector<std::byte> encoded;
    for (const auto& itm : items) {
        const auto& value = itm.value;
        size_t offset = 0;
        while (offset < value.size()) {
            size_t chunk_size = std::min(value.size() - offset, size_t{255});
            encoded.push_back(static_cast<std::byte>(itm.tag));
            encoded.push_back(static_cast<std::byte>(chunk_size));
            encoded.insert(encoded.end(), value.begin() + static_cast<ptrdiff_t>(offset),
                           value.begin() + static_cast<ptrdiff_t>(offset + chunk_size));
            offset += chunk_size;
        }
        if (value.empty()) {
            encoded.push_back(static_cast<std::byte>(itm.tag));
            encoded.push_back(std::byte{0});
        }
    }
    return encoded;
}
std::span<const std::byte> find(const std::vector<item>& items, type tag) {
    for (const auto& itm : items) {
        if (itm.tag == tag) {
            return itm.value;
        }
    }
    return {};
}
}  // namespace tlv8
struct chacha20_poly1305::impl {
    std::array<std::byte, 32> key_;
};
result<chacha20_poly1305> chacha20_poly1305::create(std::span<const std::byte, 32> key) {
    auto impl_ptr = std::make_unique<impl>();
    std::copy(key.begin(), key.end(), impl_ptr->key_.begin());
    return chacha20_poly1305{std::move(impl_ptr)};
}
result<std::vector<std::byte>> chacha20_poly1305::encrypt(std::span<const std::byte, 12> nonce,
                                                          std::span<const std::byte> plaintext,
                                                          std::span<const std::byte> aad) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: failed to create context"));
    }
    std::vector<std::byte> ciphertext(plaintext.size() + 16);
    int outlen = 0;
    int tmplen = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
                           reinterpret_cast<const unsigned char*>(impl_->key_.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: init failed"));
    }
    if (!aad.empty()) {
        if (EVP_EncryptUpdate(ctx, nullptr, &tmplen,
                              reinterpret_cast<const unsigned char*>(aad.data()),
                              static_cast<int>(aad.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: AAD failed"));
        }
    }
    if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &outlen,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: encrypt failed"));
    }
    if (EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(ciphertext.data()) + outlen,
                            &tmplen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: finalize failed"));
    }
    outlen += tmplen;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16,
                            reinterpret_cast<unsigned char*>(ciphertext.data()) + outlen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: get tag failed"));
    }
    EVP_CIPHER_CTX_free(ctx);
    ciphertext.resize(static_cast<size_t>(outlen) + 16);
    return ciphertext;
}
result<std::vector<std::byte>> chacha20_poly1305::decrypt(std::span<const std::byte, 12> nonce,
                                                          std::span<const std::byte> ciphertext,
                                                          std::span<const std::byte> aad) {
    if (ciphertext.size() < 16) {
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: ciphertext too short"));
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: failed to create context"));
    }
    size_t ct_len = ciphertext.size() - 16;
    std::vector<std::byte> plaintext(ct_len);
    int outlen = 0;
    int tmplen = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr,
                           reinterpret_cast<const unsigned char*>(impl_->key_.data()),
                           reinterpret_cast<const unsigned char*>(nonce.data())) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: init failed"));
    }
    if (!aad.empty()) {
        if (EVP_DecryptUpdate(ctx, nullptr, &tmplen,
                              reinterpret_cast<const unsigned char*>(aad.data()),
                              static_cast<int>(aad.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: AAD failed"));
        }
    }
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &outlen,
                          reinterpret_cast<const unsigned char*>(ciphertext.data()),
                          static_cast<int>(ct_len)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: decrypt failed"));
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                            const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(
                                ciphertext.data() + ct_len))) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: set tag failed"));
    }
    if (EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + outlen,
                            &tmplen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("ChaCha20-Poly1305: authentication failed"));
    }
    EVP_CIPHER_CTX_free(ctx);
    plaintext.resize(static_cast<size_t>(outlen + tmplen));
    return plaintext;
}
chacha20_poly1305::chacha20_poly1305(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
chacha20_poly1305::chacha20_poly1305(chacha20_poly1305&&) noexcept = default;
chacha20_poly1305& chacha20_poly1305::operator=(chacha20_poly1305&&) noexcept = default;
chacha20_poly1305::~chacha20_poly1305() = default;
}  // namespace mirage::crypto
