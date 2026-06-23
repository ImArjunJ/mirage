#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

#include <openssl/evp.h>

#include "core/core.hpp"
#include "crypto/crypto.hpp"

namespace mirage::crypto {

struct ed25519_keypair::impl {
    EVP_PKEY* pkey = nullptr;

    ~impl() {
        if (pkey) {
            EVP_PKEY_free(pkey);
        }
    }
};

result<ed25519_keypair> ed25519_keypair::generate() {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) {
        return std::unexpected(mirage_error::crypto("failed to create Ed25519 context"));
    }
    if (EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return std::unexpected(mirage_error::crypto("Ed25519 key generation failed"));
    }
    EVP_PKEY_CTX_free(ctx);
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->pkey = pkey;
    return ed25519_keypair{std::move(impl_ptr)};
}

result<ed25519_keypair> ed25519_keypair::from_private_key(
    std::span<const std::byte, 32> private_key) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char*>(private_key.data()), 32);
    if (!pkey) {
        return std::unexpected(mirage_error::crypto("failed to load Ed25519 private key"));
    }
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->pkey = pkey;
    return ed25519_keypair{std::move(impl_ptr)};
}

std::array<std::byte, 32> ed25519_keypair::public_key() const {
    std::array<std::byte, 32> pk{};
    size_t len = 32;
    EVP_PKEY_get_raw_public_key(impl_->pkey, reinterpret_cast<unsigned char*>(pk.data()), &len);
    return pk;
}

result<std::array<std::byte, 32>> ed25519_keypair::private_key() const {
    std::array<std::byte, 32> sk{};
    size_t len = sk.size();
    if (EVP_PKEY_get_raw_private_key(impl_->pkey, reinterpret_cast<unsigned char*>(sk.data()),
                                     &len) <= 0 ||
        len != sk.size()) {
        return std::unexpected(mirage_error::crypto("failed to export Ed25519 private key"));
    }
    return sk;
}

result<std::array<std::byte, 64>> ed25519_keypair::sign(std::span<const std::byte> message) const {
    std::array<std::byte, 64> sig{};
    size_t sig_len = 64;
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        return std::unexpected(mirage_error::crypto("failed to create signing context"));
    }
    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, impl_->pkey) <= 0 ||
        EVP_DigestSign(md_ctx, reinterpret_cast<unsigned char*>(sig.data()), &sig_len,
                       reinterpret_cast<const unsigned char*>(message.data()),
                       message.size()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        return std::unexpected(mirage_error::crypto("Ed25519 signing failed"));
    }
    EVP_MD_CTX_free(md_ctx);
    return sig;
}

result<ed25519_keypair> ed25519_keypair::clone() const {
    if (!impl_ || !impl_->pkey) {
        return std::unexpected(mirage_error::crypto("no key to clone"));
    }
    if (EVP_PKEY_up_ref(impl_->pkey) <= 0) {
        return std::unexpected(mirage_error::crypto("failed to clone Ed25519 key"));
    }
    auto new_impl = std::make_unique<impl>();
    new_impl->pkey = impl_->pkey;
    return ed25519_keypair{std::move(new_impl)};
}

ed25519_keypair::ed25519_keypair(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
ed25519_keypair::ed25519_keypair(ed25519_keypair&& other) noexcept = default;
ed25519_keypair& ed25519_keypair::operator=(ed25519_keypair&& other) noexcept = default;
ed25519_keypair::~ed25519_keypair() = default;

}  // namespace mirage::crypto
