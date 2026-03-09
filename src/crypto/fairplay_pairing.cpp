#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "core/core.hpp"
#include "core/log.hpp"
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
enum class pairing_state {
    initial,
    m1_received,
    m3_received,
    setup_complete,
    verify_m1_received,
    verified
};
struct fairplay_pairing::impl {
    ed25519_keypair keypair;
    std::unique_ptr<srp6a> srp;
    std::unique_ptr<chacha20_poly1305> session_cipher;
    std::array<std::byte, 32> session_key_{};
    std::array<std::byte, 32> shared_secret_{};
    std::array<std::byte, 32> our_setup_pubkey_{};
    std::vector<std::byte> client_public_key_;
    std::vector<std::byte> client_verify_pk_;
    std::array<std::byte, 32> verify_shared_secret_{};
    EVP_PKEY* verify_pkey_ = nullptr;
    pairing_state state_ = pairing_state::initial;
    int verify_count_ = 0;
    explicit impl(ed25519_keypair kp) : keypair(std::move(kp)) {}
    ~impl() {
        if (verify_pkey_) {
            EVP_PKEY_free(verify_pkey_);
        }
    }
};
fairplay_pairing::fairplay_pairing(ed25519_keypair keypair)
    : impl_(std::make_unique<impl>(std::move(keypair))) {}
result<std::array<std::byte, 32>> fairplay_pairing::handle_transient_setup(
    std::span<const std::byte, 32> client_pk) {
    EVP_PKEY* server_pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &server_pkey) <= 0) {
        if (ctx) {
            EVP_PKEY_CTX_free(ctx);
        }
        return std::unexpected(mirage_error::crypto("transient pairing: X25519 keygen failed"));
    }
    EVP_PKEY_CTX_free(ctx);
    std::array<std::byte, 32> server_pk{};
    size_t pk_len = 32;
    EVP_PKEY_get_raw_public_key(server_pkey, reinterpret_cast<unsigned char*>(server_pk.data()),
                                &pk_len);
    EVP_PKEY* client_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(client_pk.data()), 32);
    if (!client_pkey) {
        EVP_PKEY_free(server_pkey);
        return std::unexpected(mirage_error::crypto("transient pairing: invalid client key"));
    }
    EVP_PKEY_CTX* derive_ctx = EVP_PKEY_CTX_new(server_pkey, nullptr);
    if (!derive_ctx || EVP_PKEY_derive_init(derive_ctx) <= 0 ||
        EVP_PKEY_derive_set_peer(derive_ctx, client_pkey) <= 0) {
        EVP_PKEY_free(client_pkey);
        EVP_PKEY_free(server_pkey);
        if (derive_ctx) {
            EVP_PKEY_CTX_free(derive_ctx);
        }
        return std::unexpected(mirage_error::crypto("transient pairing: derive init failed"));
    }
    size_t shared_len = 32;
    if (EVP_PKEY_derive(derive_ctx, reinterpret_cast<unsigned char*>(impl_->shared_secret_.data()),
                        &shared_len) <= 0) {
        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(client_pkey);
        EVP_PKEY_free(server_pkey);
        return std::unexpected(mirage_error::crypto("transient pairing: derive failed"));
    }
    EVP_PKEY_CTX_free(derive_ctx);
    EVP_PKEY_free(client_pkey);
    EVP_PKEY_free(server_pkey);
    impl_->our_setup_pubkey_ = server_pk;
    impl_->state_ = pairing_state::setup_complete;
    return server_pk;
}
std::span<const std::byte, 32> fairplay_pairing::transient_shared_secret() const {
    static const std::array<std::byte, 32> zeros{};
    if (impl_->verify_shared_secret_ != zeros) {
        return impl_->verify_shared_secret_;
    }
    return impl_->shared_secret_;
}
result<std::vector<std::byte>> fairplay_pairing::handle_transient_verify1(
    std::span<const std::byte, 32> client_verify_pk,
    std::span<const std::byte, 32> client_auth_tag) {
    impl_->client_public_key_.assign(client_auth_tag.begin(), client_auth_tag.end());
    std::array<std::byte, 32> server_verify_pk{};
    {
        EVP_PKEY* server_pkey = nullptr;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_keygen(ctx, &server_pkey) <= 0) {
            if (ctx != nullptr) {
                EVP_PKEY_CTX_free(ctx);
            }
            return std::unexpected(mirage_error::crypto("pair-verify: keygen failed"));
        }
        EVP_PKEY_CTX_free(ctx);
        size_t pk_len = 32;
        EVP_PKEY_get_raw_public_key(
            server_pkey, reinterpret_cast<unsigned char*>(server_verify_pk.data()), &pk_len);
        if (impl_->verify_pkey_ != nullptr) {
            EVP_PKEY_free(impl_->verify_pkey_);
        }
        impl_->verify_pkey_ = server_pkey;
        EVP_PKEY* client_pkey = EVP_PKEY_new_raw_public_key(
            EVP_PKEY_X25519, nullptr,
            reinterpret_cast<const unsigned char*>(client_verify_pk.data()), 32);
        if (client_pkey == nullptr) {
            return std::unexpected(mirage_error::crypto("pair-verify: invalid client key"));
        }
        EVP_PKEY_CTX* derive_ctx = EVP_PKEY_CTX_new(server_pkey, nullptr);
        std::array<std::byte, 32> verify_shared{};
        if (derive_ctx == nullptr || EVP_PKEY_derive_init(derive_ctx) <= 0 ||
            EVP_PKEY_derive_set_peer(derive_ctx, client_pkey) <= 0) {
            EVP_PKEY_free(client_pkey);
            if (derive_ctx != nullptr) {
                EVP_PKEY_CTX_free(derive_ctx);
            }
            return std::unexpected(mirage_error::crypto("pair-verify: derive init failed"));
        }
        size_t shared_len = 32;
        EVP_PKEY_derive(derive_ctx, reinterpret_cast<unsigned char*>(verify_shared.data()),
                        &shared_len);
        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(client_pkey);
        impl_->verify_shared_secret_ = verify_shared;
    }
    impl_->client_verify_pk_.assign(client_verify_pk.begin(), client_verify_pk.end());
    impl_->verify_count_++;
    bool is_verify_after_setup = (impl_->state_ == pairing_state::setup_complete);
    mirage::log::debug("Transient verify: verify_count={}, setup_complete={}", impl_->verify_count_,
                       is_verify_after_setup);
    std::vector<std::byte> response;
    response.reserve(96);
    response.insert(response.end(), server_verify_pk.begin(), server_verify_pk.end());
    std::vector<std::byte> sign_data;
    sign_data.reserve(64);
    sign_data.insert(sign_data.end(), server_verify_pk.begin(), server_verify_pk.end());
    sign_data.insert(sign_data.end(), client_verify_pk.begin(), client_verify_pk.end());
    auto sig_result = impl_->keypair.sign(sign_data);
    if (!sig_result) {
        return std::unexpected(sig_result.error());
    }
    std::array<std::byte, 16> aes_key{};
    std::array<std::byte, 16> aes_iv{};
    {
        std::string_view key_salt = "Pair-Verify-AES-Key";
        std::array<std::byte, 64> hash{};
        unsigned int hash_len = 0;
        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md_ctx, key_salt.data(), key_salt.size());
        EVP_DigestUpdate(md_ctx, impl_->verify_shared_secret_.data(),
                         impl_->verify_shared_secret_.size());
        EVP_DigestFinal_ex(md_ctx, reinterpret_cast<unsigned char*>(hash.data()), &hash_len);
        EVP_MD_CTX_free(md_ctx);
        std::memcpy(aes_key.data(), hash.data(), 16);
    }
    {
        std::string_view iv_salt = "Pair-Verify-AES-IV";
        std::array<std::byte, 64> hash{};
        unsigned int hash_len = 0;
        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md_ctx, iv_salt.data(), iv_salt.size());
        EVP_DigestUpdate(md_ctx, impl_->verify_shared_secret_.data(),
                         impl_->verify_shared_secret_.size());
        EVP_DigestFinal_ex(md_ctx, reinterpret_cast<unsigned char*>(hash.data()), &hash_len);
        EVP_MD_CTX_free(md_ctx);
        std::memcpy(aes_iv.data(), hash.data(), 16);
    }
    std::array<std::byte, 64> encrypted_sig{};
    auto encryptor_result = aes_ctr_encryptor::create(aes_key, aes_iv);
    if (encryptor_result) {
        auto _ = encryptor_result->encrypt(*sig_result, encrypted_sig);
    } else {
        return std::unexpected(encryptor_result.error());
    }
    response.insert(response.end(), encrypted_sig.begin(), encrypted_sig.end());
    mirage::log::debug("Pair-verify: sending X25519 ephemeral + encrypted Ed25519 signature");
    if (is_verify_after_setup) {
        std::array<std::byte, 64> combined_secret{};
        std::memcpy(combined_secret.data(), impl_->shared_secret_.data(), 32);
        std::memcpy(combined_secret.data() + 32, impl_->verify_shared_secret_.data(), 32);
        std::array<std::byte, 32> derived_key{};
        auto hkdf_result = hkdf_derive(combined_secret, std::span<const std::byte>{},
                                       std::span<const std::byte>{}, derived_key);
        if (hkdf_result) {
            impl_->session_key_ = derived_key;
            mirage::log::debug("Derived session key from combined secrets");
        }
    }
    return response;
}
result<void> fairplay_pairing::handle_transient_verify2(
    std::span<const std::byte, 64> client_signature) {
    std::array<std::byte, 16> aes_key{};
    std::array<std::byte, 16> aes_iv{};
    {
        std::string_view key_salt = "Pair-Verify-AES-Key";
        std::array<std::byte, 64> hash{};
        unsigned int hash_len = 0;
        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md_ctx, key_salt.data(), key_salt.size());
        EVP_DigestUpdate(md_ctx, impl_->verify_shared_secret_.data(),
                         impl_->verify_shared_secret_.size());
        EVP_DigestFinal_ex(md_ctx, reinterpret_cast<unsigned char*>(hash.data()), &hash_len);
        EVP_MD_CTX_free(md_ctx);
        std::memcpy(aes_key.data(), hash.data(), 16);
    }
    {
        std::string_view iv_salt = "Pair-Verify-AES-IV";
        std::array<std::byte, 64> hash{};
        unsigned int hash_len = 0;
        EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(md_ctx, EVP_sha512(), nullptr);
        EVP_DigestUpdate(md_ctx, iv_salt.data(), iv_salt.size());
        EVP_DigestUpdate(md_ctx, impl_->verify_shared_secret_.data(),
                         impl_->verify_shared_secret_.size());
        EVP_DigestFinal_ex(md_ctx, reinterpret_cast<unsigned char*>(hash.data()), &hash_len);
        EVP_MD_CTX_free(md_ctx);
        std::memcpy(aes_iv.data(), hash.data(), 16);
    }
    std::array<std::byte, 64> decrypted_sig{};
    std::array<std::byte, 64> fake_round{};
    EVP_CIPHER_CTX* cipher_ctx = EVP_CIPHER_CTX_new();
    if (!cipher_ctx) {
        return std::unexpected(
            mirage_error::crypto("pair-verify2: cipher context creation failed"));
    }
    if (EVP_DecryptInit_ex(cipher_ctx, EVP_aes_128_ctr(), nullptr,
                           reinterpret_cast<const unsigned char*>(aes_key.data()),
                           reinterpret_cast<const unsigned char*>(aes_iv.data())) != 1) {
        EVP_CIPHER_CTX_free(cipher_ctx);
        return std::unexpected(mirage_error::crypto("pair-verify2: cipher init failed"));
    }
    int outlen = 0;
    EVP_DecryptUpdate(cipher_ctx, reinterpret_cast<unsigned char*>(fake_round.data()), &outlen,
                      reinterpret_cast<const unsigned char*>(fake_round.data()), 64);
    EVP_DecryptUpdate(cipher_ctx, reinterpret_cast<unsigned char*>(decrypted_sig.data()), &outlen,
                      reinterpret_cast<const unsigned char*>(client_signature.data()), 64);
    EVP_CIPHER_CTX_free(cipher_ctx);
    std::array<std::byte, 64> expected_msg{};
    std::memcpy(expected_msg.data(), impl_->client_verify_pk_.data(), 32);
    std::array<std::byte, 32> our_verify_pk{};
    size_t pk_len = 32;
    EVP_PKEY_get_raw_public_key(impl_->verify_pkey_,
                                reinterpret_cast<unsigned char*>(our_verify_pk.data()), &pk_len);
    std::memcpy(expected_msg.data() + 32, our_verify_pk.data(), 32);
    EVP_PKEY* client_ed_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(impl_->client_public_key_.data()), 32);
    if (!client_ed_pkey) {
        return std::unexpected(mirage_error::crypto("pair-verify2: invalid client Ed25519 key"));
    }
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(client_ed_pkey);
        return std::unexpected(mirage_error::crypto("pair-verify2: MD context creation failed"));
    }
    int verify_result = 0;
    if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, client_ed_pkey) == 1) {
        verify_result =
            EVP_DigestVerify(md_ctx, reinterpret_cast<const unsigned char*>(decrypted_sig.data()),
                             64, reinterpret_cast<const unsigned char*>(expected_msg.data()), 64);
    }
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(client_ed_pkey);
    if (verify_result != 1) {
        mirage::log::warn("pair-verify2: signature verification failed");
        return std::unexpected(mirage_error::crypto("pair-verify2: signature verification failed"));
    }
    mirage::log::debug("pair-verify2: client signature verified successfully");
    impl_->state_ = pairing_state::verified;
    return {};
}
result<std::vector<std::byte>> fairplay_pairing::handle_m1(std::span<const std::byte> data) {
    auto items_result = tlv8::parse(data);
    if (!items_result) {
        return std::unexpected(items_result.error());
    }
    auto& items = *items_result;
    auto state_data = tlv8::find(items, tlv8::type::state);
    if (state_data.empty() || static_cast<uint8_t>(state_data[0]) != 1) {
        return std::unexpected(mirage_error::crypto("pair-setup: expected M1 state"));
    }
    auto method = tlv8::find(items, tlv8::type::method);
    if (!method.empty() && static_cast<uint8_t>(method[0]) != 0) {
        return std::unexpected(mirage_error::crypto("pair-setup: unsupported method"));
    }
    auto srp_result = srp6a::create_server("Pair-Setup", "3939");
    if (!srp_result) {
        return std::unexpected(srp_result.error());
    }
    impl_->srp = std::make_unique<srp6a>(std::move(*srp_result));
    auto salt = impl_->srp->get_salt();
    auto pk = impl_->srp->get_public_key();
    std::vector<tlv8::item> response_items = {
        {tlv8::type::state, {std::byte{2}}},
        {tlv8::type::salt, std::vector<std::byte>(salt.begin(), salt.end())},
        {tlv8::type::public_key, std::vector<std::byte>(pk.begin(), pk.end())},
    };
    impl_->state_ = pairing_state::m1_received;
    return tlv8::encode(response_items);
}
result<std::vector<std::byte>> fairplay_pairing::handle_m3(std::span<const std::byte> data) {
    if (impl_->state_ != pairing_state::m1_received || !impl_->srp) {
        return std::unexpected(mirage_error::crypto("pair-setup: invalid state for M3"));
    }
    auto items_result = tlv8::parse(data);
    if (!items_result) {
        return std::unexpected(items_result.error());
    }
    auto& items = *items_result;
    auto state_data = tlv8::find(items, tlv8::type::state);
    if (state_data.empty() || static_cast<uint8_t>(state_data[0]) != 3) {
        return std::unexpected(mirage_error::crypto("pair-setup: expected M3 state"));
    }
    auto client_pk = tlv8::find(items, tlv8::type::public_key);
    auto client_proof = tlv8::find(items, tlv8::type::proof);
    if (client_pk.empty()) {
        return std::unexpected(mirage_error::crypto("pair-setup M3: missing public key"));
    }
    if (client_proof.empty()) {
        return std::unexpected(mirage_error::crypto("pair-setup M3: missing proof"));
    }
    mirage::log::debug("M3 client public key: {} bytes", client_pk.size());
    mirage::log::debug("M3 client proof: {} bytes", client_proof.size());
    auto compute_result = impl_->srp->compute_session_key(client_pk);
    if (!compute_result) {
        return std::unexpected(compute_result.error());
    }
    auto verify_result = impl_->srp->verify_client_proof(client_proof);
    if (!verify_result || !*verify_result) {
        return std::unexpected(mirage_error::crypto("pair-setup M3: proof verification failed"));
    }
    auto server_proof = impl_->srp->generate_server_proof();
    std::string proof_hex;
    for (const auto& b : server_proof) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02x ", static_cast<uint8_t>(b));
        proof_hex += buf;
    }
    mirage::log::debug("M4 server proof ({} bytes): {}", server_proof.size(), proof_hex);
    std::vector<tlv8::item> response_items = {
        {tlv8::type::state, {std::byte{4}}},
        {tlv8::type::proof, std::move(server_proof)},
    };
    auto encoded = tlv8::encode(response_items);
    mirage::log::debug("M4 response size: {} bytes", encoded.size());
    impl_->state_ = pairing_state::m3_received;
    return encoded;
}
result<std::vector<std::byte>> fairplay_pairing::handle_m5(std::span<const std::byte> data) {
    if (impl_->state_ != pairing_state::m3_received || !impl_->srp) {
        return std::unexpected(mirage_error::crypto("pair-setup: invalid state for M5"));
    }
    auto items_result = tlv8::parse(data);
    if (!items_result) {
        return std::unexpected(items_result.error());
    }
    auto& items = *items_result;
    auto state_data = tlv8::find(items, tlv8::type::state);
    if (state_data.empty() || static_cast<uint8_t>(state_data[0]) != 5) {
        return std::unexpected(mirage_error::crypto("pair-setup: expected M5 state"));
    }
    auto encrypted_data = tlv8::find(items, tlv8::type::encrypted_data);
    if (encrypted_data.empty()) {
        return std::unexpected(mirage_error::crypto("pair-setup M5: missing encrypted data"));
    }
    mirage::log::debug("M5 encrypted data: {} bytes", encrypted_data.size());
    auto srp_session_key = impl_->srp->get_session_key();
    mirage::log::debug("SRP session key: {} bytes", srp_session_key.size());
    std::array<std::byte, 32> enc_key{};
    std::string_view salt_str = "Pair-Setup-Encrypt-Salt";
    std::string_view info_str = "Pair-Setup-Encrypt-Info";
    auto hkdf_result =
        hkdf_derive(srp_session_key,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(salt_str.data()),
                                               salt_str.size()),
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(info_str.data()),
                                               info_str.size()),
                    enc_key);
    if (!hkdf_result) {
        return std::unexpected(hkdf_result.error());
    }
    auto cipher_result = chacha20_poly1305::create(enc_key);
    if (!cipher_result) {
        return std::unexpected(cipher_result.error());
    }
    std::array<std::byte, 12> nonce{};
    std::memcpy(nonce.data() + 4, "PS-Msg05", 8);
    auto decrypted = cipher_result->decrypt(
        nonce, std::vector<std::byte>(encrypted_data.begin(), encrypted_data.end()));
    if (!decrypted) {
        mirage::log::warn("M5 decryption failed: {}", decrypted.error().message);
    } else {
        mirage::log::debug("M5 decrypted payload: {} bytes", decrypted->size());
    }
    auto ed_pk = impl_->keypair.public_key();
    std::array<std::byte, 32> accessory_x{};
    std::string_view sign_salt = "Pair-Setup-Accessory-Sign-Salt";
    std::string_view sign_info = "Pair-Setup-Accessory-Sign-Info";
    auto sign_hkdf_result =
        hkdf_derive(srp_session_key,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(sign_salt.data()),
                                               sign_salt.size()),
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(sign_info.data()),
                                               sign_info.size()),
                    accessory_x);
    if (!sign_hkdf_result) {
        return std::unexpected(sign_hkdf_result.error());
    }
    std::string_view accessory_id = "Mirage";
    std::vector<std::byte> sign_data;
    sign_data.insert(sign_data.end(), accessory_x.begin(), accessory_x.end());
    sign_data.insert(sign_data.end(), reinterpret_cast<const std::byte*>(accessory_id.data()),
                     reinterpret_cast<const std::byte*>(accessory_id.data() + accessory_id.size()));
    sign_data.insert(sign_data.end(), ed_pk.begin(), ed_pk.end());
    auto sig_result = impl_->keypair.sign(sign_data);
    if (!sig_result) {
        return std::unexpected(sig_result.error());
    }
    std::vector<tlv8::item> sub_items = {
        {tlv8::type::identifier,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(accessory_id.data()),
             reinterpret_cast<const std::byte*>(accessory_id.data() + accessory_id.size()))},
        {tlv8::type::public_key, std::vector<std::byte>(ed_pk.begin(), ed_pk.end())},
        {tlv8::type::signature, std::vector<std::byte>(sig_result->begin(), sig_result->end())},
    };
    auto sub_tlv = tlv8::encode(sub_items);
    std::array<std::byte, 12> m6_nonce{};
    std::memcpy(m6_nonce.data() + 4, "PS-Msg06", 8);
    auto encrypted = cipher_result->encrypt(m6_nonce, sub_tlv);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    std::vector<tlv8::item> response_items = {
        {tlv8::type::state, {std::byte{6}}},
        {tlv8::type::encrypted_data, std::move(*encrypted)},
    };
    mirage::log::debug("M6 response ready");
    impl_->state_ = pairing_state::setup_complete;
    return tlv8::encode(response_items);
}
result<std::vector<std::byte>> fairplay_pairing::handle_verify_m1(std::span<const std::byte> data) {
    auto items_result = tlv8::parse(data);
    if (!items_result) {
        return std::unexpected(items_result.error());
    }
    auto& items = *items_result;
    auto state_data = tlv8::find(items, tlv8::type::state);
    if (state_data.empty() || static_cast<uint8_t>(state_data[0]) != 1) {
        return std::unexpected(mirage_error::crypto("pair-verify: expected M1 state"));
    }
    auto client_curve_pk = tlv8::find(items, tlv8::type::public_key);
    if (client_curve_pk.size() != 32) {
        return std::unexpected(mirage_error::crypto("pair-verify M1: invalid public key size"));
    }
    impl_->client_public_key_.assign(client_curve_pk.begin(), client_curve_pk.end());
    EVP_PKEY* server_curve_pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_keygen(ctx, &server_curve_pkey) <= 0) {
        if (ctx) {
            EVP_PKEY_CTX_free(ctx);
        }
        return std::unexpected(mirage_error::crypto("pair-verify: X25519 keygen failed"));
    }
    EVP_PKEY_CTX_free(ctx);
    std::array<std::byte, 32> server_curve_pk{};
    size_t pk_len = 32;
    EVP_PKEY_get_raw_public_key(server_curve_pkey,
                                reinterpret_cast<unsigned char*>(server_curve_pk.data()), &pk_len);
    EVP_PKEY* client_pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_X25519, nullptr, reinterpret_cast<const unsigned char*>(client_curve_pk.data()),
        32);
    if (!client_pkey) {
        EVP_PKEY_free(server_curve_pkey);
        return std::unexpected(mirage_error::crypto("pair-verify: invalid client key"));
    }
    EVP_PKEY_CTX* derive_ctx = EVP_PKEY_CTX_new(server_curve_pkey, nullptr);
    if (!derive_ctx || EVP_PKEY_derive_init(derive_ctx) <= 0 ||
        EVP_PKEY_derive_set_peer(derive_ctx, client_pkey) <= 0) {
        EVP_PKEY_free(client_pkey);
        EVP_PKEY_free(server_curve_pkey);
        if (derive_ctx) {
            EVP_PKEY_CTX_free(derive_ctx);
        }
        return std::unexpected(mirage_error::crypto("pair-verify: derive init failed"));
    }
    size_t shared_len = 32;
    if (EVP_PKEY_derive(derive_ctx, reinterpret_cast<unsigned char*>(impl_->shared_secret_.data()),
                        &shared_len) <= 0) {
        EVP_PKEY_CTX_free(derive_ctx);
        EVP_PKEY_free(client_pkey);
        EVP_PKEY_free(server_curve_pkey);
        return std::unexpected(mirage_error::crypto("pair-verify: derive failed"));
    }
    EVP_PKEY_CTX_free(derive_ctx);
    EVP_PKEY_free(client_pkey);
    EVP_PKEY_free(server_curve_pkey);
    std::array<std::byte, 32> session_enc_key{};
    std::string_view info = "Pair-Verify-Encrypt-Info";
    std::string_view salt = "Pair-Verify-Encrypt-Salt";
    auto hkdf_res = hkdf_derive(
        impl_->shared_secret_,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(salt.data()), salt.size()),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(info.data()), info.size()),
        session_enc_key);
    if (!hkdf_res) {
        return std::unexpected(hkdf_res.error());
    }
    auto cipher_result = chacha20_poly1305::create(session_enc_key);
    if (!cipher_result) {
        return std::unexpected(cipher_result.error());
    }
    impl_->session_cipher = std::make_unique<chacha20_poly1305>(std::move(*cipher_result));
    std::array<std::byte, 32> accessory_info{};
    std::string_view sign_salt = "Pair-Verify-Accessory-Sign-Salt";
    std::string_view sign_info_str = "Pair-Verify-Accessory-Sign-Info";
    auto sign_hkdf_result = hkdf_derive(
        impl_->shared_secret_,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(sign_salt.data()),
                                   sign_salt.size()),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(sign_info_str.data()),
                                   sign_info_str.size()),
        accessory_info);
    if (!sign_hkdf_result) {
        return std::unexpected(sign_hkdf_result.error());
    }
    std::string_view accessory_id = "Mirage";
    auto ed_pk = impl_->keypair.public_key();
    std::vector<std::byte> sign_data;
    sign_data.insert(sign_data.end(), accessory_info.begin(), accessory_info.end());
    sign_data.insert(sign_data.end(), reinterpret_cast<const std::byte*>(accessory_id.data()),
                     reinterpret_cast<const std::byte*>(accessory_id.data() + accessory_id.size()));
    sign_data.insert(sign_data.end(), ed_pk.begin(), ed_pk.end());
    auto sig_result = impl_->keypair.sign(sign_data);
    if (!sig_result) {
        return std::unexpected(sig_result.error());
    }
    std::vector<tlv8::item> sub_items = {
        {tlv8::type::identifier,
         std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(accessory_id.data()),
             reinterpret_cast<const std::byte*>(accessory_id.data() + accessory_id.size()))},
        {tlv8::type::signature, std::vector<std::byte>(sig_result->begin(), sig_result->end())},
    };
    auto sub_tlv = tlv8::encode(sub_items);
    std::array<std::byte, 12> nonce{};
    std::memcpy(nonce.data() + 4, "PV-Msg02", 8);
    auto encrypted = impl_->session_cipher->encrypt(nonce, sub_tlv);
    if (!encrypted) {
        return std::unexpected(encrypted.error());
    }
    std::vector<tlv8::item> response_items = {
        {tlv8::type::state, {std::byte{2}}},
        {tlv8::type::public_key,
         std::vector<std::byte>(server_curve_pk.begin(), server_curve_pk.end())},
        {tlv8::type::encrypted_data, std::move(*encrypted)},
    };
    impl_->state_ = pairing_state::verify_m1_received;
    return tlv8::encode(response_items);
}
result<std::array<std::byte, 32>> fairplay_pairing::handle_verify_m3(
    std::span<const std::byte> data) {
    if (impl_->state_ != pairing_state::verify_m1_received || !impl_->session_cipher) {
        return std::unexpected(mirage_error::crypto("pair-verify: invalid state for M3"));
    }
    auto items_result = tlv8::parse(data);
    if (!items_result) {
        return std::unexpected(items_result.error());
    }
    auto& items = *items_result;
    auto state_data = tlv8::find(items, tlv8::type::state);
    if (state_data.empty() || static_cast<uint8_t>(state_data[0]) != 3) {
        return std::unexpected(mirage_error::crypto("pair-verify: expected M3 state"));
    }
    auto encrypted_data = tlv8::find(items, tlv8::type::encrypted_data);
    if (encrypted_data.empty()) {
        return std::unexpected(mirage_error::crypto("pair-verify M3: missing encrypted data"));
    }
    std::array<std::byte, 12> nonce{};
    std::memcpy(nonce.data() + 4, "PV-Msg03", 8);
    auto decrypted = impl_->session_cipher->decrypt(
        nonce, std::vector<std::byte>(encrypted_data.begin(), encrypted_data.end()));
    if (!decrypted) {
        return std::unexpected(mirage_error::crypto("pair-verify M3: decryption failed"));
    }
    auto inner_items_result = tlv8::parse(*decrypted);
    if (!inner_items_result) {
        return std::unexpected(inner_items_result.error());
    }
    std::string_view info = "Pair-Verify-AES-Key-Info";
    std::string_view salt = "Pair-Verify-AES-Key-Salt";
    auto hkdf_res = hkdf_derive(
        impl_->shared_secret_,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(salt.data()), salt.size()),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(info.data()), info.size()),
        impl_->session_key_);
    if (!hkdf_res) {
        return std::unexpected(hkdf_res.error());
    }
    return impl_->session_key_;
}
std::span<const std::byte, 32> fairplay_pairing::session_key() const {
    return impl_->session_key_;
}
bool fairplay_pairing::is_setup_complete() const {
    return impl_->state_ == pairing_state::setup_complete;
}
void fairplay_pairing::set_setup_complete() {
    impl_->state_ = pairing_state::setup_complete;
}
std::array<std::byte, 32> fairplay_pairing::ed25519_public_key() const {
    return impl_->keypair.public_key();
}
fairplay_pairing::fairplay_pairing(fairplay_pairing&&) noexcept = default;
fairplay_pairing& fairplay_pairing::operator=(fairplay_pairing&&) noexcept = default;
fairplay_pairing::~fairplay_pairing() = default;
}  // namespace mirage::crypto
