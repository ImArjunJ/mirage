#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/sha.h>

#include "core/core.hpp"
#include "crypto/crypto.hpp"
namespace mirage::crypto {
void sap_decrypt(const uint8_t* message, const uint8_t* cipher, uint8_t* key_out);
struct aes_ctr_decryptor::impl {
    EVP_CIPHER_CTX* ctx = nullptr;
    std::array<std::byte, 16> key_{};
    std::array<std::byte, 16> iv_{};
    ~impl() {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
result<aes_ctr_decryptor> aes_ctr_decryptor::create(std::span<const std::byte, 16> key,
                                                    std::span<const std::byte, 16> iv) {
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->ctx = EVP_CIPHER_CTX_new();
    if (!impl_ptr->ctx) {
        return std::unexpected(mirage_error::crypto("failed to create cipher context"));
    }
    std::memcpy(impl_ptr->key_.data(), key.data(), 16);
    std::memcpy(impl_ptr->iv_.data(), iv.data(), 16);
    if (EVP_EncryptInit_ex(impl_ptr->ctx, EVP_aes_128_ctr(), nullptr,
                           reinterpret_cast<const unsigned char*>(impl_ptr->key_.data()),
                           reinterpret_cast<const unsigned char*>(impl_ptr->iv_.data())) != 1) {
        return std::unexpected(mirage_error::crypto("failed to initialize AES-CTR"));
    }
    EVP_CIPHER_CTX_set_padding(impl_ptr->ctx, 0);
    return aes_ctr_decryptor{std::move(impl_ptr)};
}
result<size_t> aes_ctr_decryptor::decrypt(std::span<const std::byte> ciphertext,
                                          std::span<std::byte> plaintext) {
    if (plaintext.size() < ciphertext.size()) {
        return std::unexpected(mirage_error::crypto("output buffer too small"));
    }
    int outlen = 0;
    if (EVP_EncryptUpdate(impl_->ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &outlen,
                          reinterpret_cast<const unsigned char*>(ciphertext.data()),
                          static_cast<int>(ciphertext.size())) != 1) {
        return std::unexpected(mirage_error::crypto("AES-CTR decryption failed"));
    }
    return static_cast<size_t>(outlen);
}
result<void> aes_ctr_decryptor::set_key(std::span<const std::byte> key) {
    if (key.size() != 16) {
        return std::unexpected(mirage_error::crypto("AES key must be 16 bytes"));
    }
    std::memcpy(impl_->key_.data(), key.data(), 16);
    if (EVP_EncryptInit_ex(impl_->ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(impl_->key_.data()),
                           nullptr) != 1) {
        return std::unexpected(mirage_error::crypto("failed to set AES key"));
    }
    return {};
}
result<void> aes_ctr_decryptor::reset() {
    if (EVP_EncryptInit_ex(impl_->ctx, EVP_aes_128_ctr(), nullptr,
                           reinterpret_cast<const unsigned char*>(impl_->key_.data()),
                           reinterpret_cast<const unsigned char*>(impl_->iv_.data())) != 1) {
        return std::unexpected(mirage_error::crypto("failed to reset AES-CTR"));
    }
    EVP_CIPHER_CTX_set_padding(impl_->ctx, 0);
    return {};
}
aes_ctr_decryptor::aes_ctr_decryptor(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
aes_ctr_decryptor::aes_ctr_decryptor(aes_ctr_decryptor&&) noexcept = default;
aes_ctr_decryptor& aes_ctr_decryptor::operator=(aes_ctr_decryptor&&) noexcept = default;
aes_ctr_decryptor::~aes_ctr_decryptor() = default;
result<size_t> aes_cbc_decrypt(std::span<const std::byte, 16> key,
                               std::span<const std::byte, 16> iv, std::span<const std::byte> input,
                               std::span<std::byte> output) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return std::unexpected(mirage_error::crypto("AES-CBC context creation failed"));
    }
    EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr,
                       reinterpret_cast<const unsigned char*>(key.data()),
                       reinterpret_cast<const unsigned char*>(iv.data()));
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen = 0;
    EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(output.data()), &outlen,
                      reinterpret_cast<const unsigned char*>(input.data()),
                      static_cast<int>(input.size()));
    int finallen = 0;
    EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(output.data()) + outlen, &finallen);
    EVP_CIPHER_CTX_free(ctx);
    return static_cast<size_t>(outlen + finallen);
}
result<void> hkdf_derive(std::span<const std::byte> ikm, std::span<const std::byte> salt,
                         std::span<const std::byte> info, std::span<std::byte> okm) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx) {
        return std::unexpected(mirage_error::crypto("HKDF context creation failed"));
    }
    if (EVP_PKEY_derive_init(ctx) <= 0 || EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha512()) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("HKDF init failed"));
    }
    if (!salt.empty()) {
        if (EVP_PKEY_CTX_set1_hkdf_salt(ctx, reinterpret_cast<const unsigned char*>(salt.data()),
                                        static_cast<int>(salt.size())) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return std::unexpected(mirage_error::crypto("HKDF salt failed"));
        }
    }
    if (EVP_PKEY_CTX_set1_hkdf_key(ctx, reinterpret_cast<const unsigned char*>(ikm.data()),
                                   static_cast<int>(ikm.size())) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("HKDF key failed"));
    }
    if (!info.empty()) {
        if (EVP_PKEY_CTX_add1_hkdf_info(ctx, reinterpret_cast<const unsigned char*>(info.data()),
                                        static_cast<int>(info.size())) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return std::unexpected(mirage_error::crypto("HKDF info failed"));
        }
    }
    size_t outlen = okm.size();
    if (EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(okm.data()), &outlen) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("HKDF derive failed"));
    }
    EVP_PKEY_CTX_free(ctx);
    return {};
}
struct aes_ctr_encryptor::impl {
    EVP_CIPHER_CTX* ctx;
    impl() : ctx(EVP_CIPHER_CTX_new()) {}
    ~impl() {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
result<aes_ctr_encryptor> aes_ctr_encryptor::create(std::span<const std::byte, 16> key,
                                                    std::span<const std::byte, 16> iv) {
    auto impl_ptr = std::make_unique<impl>();
    if (!impl_ptr->ctx) {
        return std::unexpected(mirage_error::crypto("failed to create AES encryptor context"));
    }
    if (EVP_EncryptInit_ex(impl_ptr->ctx, EVP_aes_128_ctr(), nullptr,
                           reinterpret_cast<const unsigned char*>(key.data()),
                           reinterpret_cast<const unsigned char*>(iv.data())) <= 0) {
        return std::unexpected(mirage_error::crypto("failed to initialize AES-CTR encryptor"));
    }
    return aes_ctr_encryptor{std::move(impl_ptr)};
}
result<size_t> aes_ctr_encryptor::encrypt(std::span<const std::byte> plaintext,
                                          std::span<std::byte> ciphertext) {
    if (plaintext.size() > ciphertext.size()) {
        return std::unexpected(mirage_error::crypto("ciphertext buffer too small"));
    }
    int out_len = 0;
    if (EVP_EncryptUpdate(impl_->ctx, reinterpret_cast<unsigned char*>(ciphertext.data()), &out_len,
                          reinterpret_cast<const unsigned char*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) <= 0) {
        return std::unexpected(mirage_error::crypto("AES-CTR encryption failed"));
    }
    return static_cast<size_t>(out_len);
}
aes_ctr_encryptor::aes_ctr_encryptor(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
aes_ctr_encryptor::aes_ctr_encryptor(aes_ctr_encryptor&&) noexcept = default;
aes_ctr_encryptor& aes_ctr_encryptor::operator=(aes_ctr_encryptor&&) noexcept = default;
aes_ctr_encryptor::~aes_ctr_encryptor() = default;
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
std::array<std::byte, 16> fairplay_decrypt_key(std::span<const std::byte, 164> keymsg,
                                               std::span<const std::byte, 72> ekey) {
    std::array<std::byte, 16> aeskey{};
    sap_decrypt(reinterpret_cast<const uint8_t*>(keymsg.data()),
                reinterpret_cast<const uint8_t*>(ekey.data()),
                reinterpret_cast<uint8_t*>(aeskey.data()));
    return aeskey;
}
}  // namespace mirage::crypto
