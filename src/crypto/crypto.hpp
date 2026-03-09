#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"
namespace mirage::crypto {
class ed25519_keypair {
public:
    static result<ed25519_keypair> generate();
    static result<ed25519_keypair> from_private_key(std::span<const std::byte, 32> private_key);
    [[nodiscard]] std::array<std::byte, 32> public_key() const;
    [[nodiscard]] result<std::array<std::byte, 64>> sign(std::span<const std::byte> message) const;
    [[nodiscard]] result<ed25519_keypair> clone() const;
    ed25519_keypair(const ed25519_keypair&) = delete;
    ed25519_keypair& operator=(const ed25519_keypair&) = delete;
    ed25519_keypair(ed25519_keypair&&) noexcept;
    ed25519_keypair& operator=(ed25519_keypair&&) noexcept;
    ~ed25519_keypair();

private:
    struct impl;
    explicit ed25519_keypair(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
class aes_ctr_decryptor {
public:
    static result<aes_ctr_decryptor> create(std::span<const std::byte, 16> key,
                                            std::span<const std::byte, 16> iv);
    result<size_t> decrypt(std::span<const std::byte> ciphertext, std::span<std::byte> plaintext);
    result<void> set_key(std::span<const std::byte> key);
    result<void> reset();
    aes_ctr_decryptor(const aes_ctr_decryptor&) = delete;
    aes_ctr_decryptor& operator=(const aes_ctr_decryptor&) = delete;
    aes_ctr_decryptor(aes_ctr_decryptor&&) noexcept;
    aes_ctr_decryptor& operator=(aes_ctr_decryptor&&) noexcept;
    ~aes_ctr_decryptor();

private:
    struct impl;
    explicit aes_ctr_decryptor(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
class aes_ctr_encryptor {
public:
    static result<aes_ctr_encryptor> create(std::span<const std::byte, 16> key,
                                            std::span<const std::byte, 16> iv);
    result<size_t> encrypt(std::span<const std::byte> plaintext, std::span<std::byte> ciphertext);
    aes_ctr_encryptor(const aes_ctr_encryptor&) = delete;
    aes_ctr_encryptor& operator=(const aes_ctr_encryptor&) = delete;
    aes_ctr_encryptor(aes_ctr_encryptor&&) noexcept;
    aes_ctr_encryptor& operator=(aes_ctr_encryptor&&) noexcept;
    ~aes_ctr_encryptor();

private:
    struct impl;
    explicit aes_ctr_encryptor(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
result<size_t> aes_cbc_decrypt(std::span<const std::byte, 16> key,
                               std::span<const std::byte, 16> iv, std::span<const std::byte> input,
                               std::span<std::byte> output);
result<void> hkdf_derive(std::span<const std::byte> ikm, std::span<const std::byte> salt,
                         std::span<const std::byte> info, std::span<std::byte> okm);
std::array<std::byte, 64> sha512(std::span<const std::byte> data);
std::array<std::byte, 64> sha512_concat(std::string_view prefix, uint64_t stream_id,
                                        std::span<const std::byte> key);
std::array<std::byte, 16> fairplay_decrypt_key(std::span<const std::byte, 164> keymsg,
                                               std::span<const std::byte, 72> ekey);
namespace tlv8 {
enum class type : uint8_t {
    method = 0x00,
    identifier = 0x01,
    salt = 0x02,
    public_key = 0x03,
    proof = 0x04,
    encrypted_data = 0x05,
    state = 0x06,
    error = 0x07,
    signature = 0x0A,
};
struct item {
    type tag;
    std::vector<std::byte> value;
};
result<std::vector<item>> parse(std::span<const std::byte> data);
std::vector<std::byte> encode(std::span<const item> items);
std::span<const std::byte> find(const std::vector<item>& items, type tag);
}  // namespace tlv8
class chacha20_poly1305 {
public:
    static result<chacha20_poly1305> create(std::span<const std::byte, 32> key);
    result<std::vector<std::byte>> encrypt(std::span<const std::byte, 12> nonce,
                                           std::span<const std::byte> plaintext,
                                           std::span<const std::byte> aad = {});
    result<std::vector<std::byte>> decrypt(std::span<const std::byte, 12> nonce,
                                           std::span<const std::byte> ciphertext,
                                           std::span<const std::byte> aad = {});
    chacha20_poly1305(const chacha20_poly1305&) = delete;
    chacha20_poly1305& operator=(const chacha20_poly1305&) = delete;
    chacha20_poly1305(chacha20_poly1305&&) noexcept;
    chacha20_poly1305& operator=(chacha20_poly1305&&) noexcept;
    ~chacha20_poly1305();

private:
    struct impl;
    explicit chacha20_poly1305(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
class fairplay_pairing {
public:
    explicit fairplay_pairing(ed25519_keypair keypair);
    result<std::array<std::byte, 32>> handle_transient_setup(
        std::span<const std::byte, 32> client_pk);
    [[nodiscard]] std::span<const std::byte, 32> transient_shared_secret() const;
    result<std::vector<std::byte>> handle_transient_verify1(
        std::span<const std::byte, 32> client_verify_pk,
        std::span<const std::byte, 32> client_auth_tag);
    result<std::vector<std::byte>> handle_m1(std::span<const std::byte> client_pk);
    result<std::vector<std::byte>> handle_m3(std::span<const std::byte> client_proof);
    result<std::vector<std::byte>> handle_m5(std::span<const std::byte> encrypted_data);
    result<std::vector<std::byte>> handle_verify_m1(std::span<const std::byte> encrypted_data);
    result<std::array<std::byte, 32>> handle_verify_m3(std::span<const std::byte> encrypted_data);
    [[nodiscard]] std::span<const std::byte, 32> session_key() const;
    [[nodiscard]] bool is_setup_complete() const;
    void set_setup_complete();
    result<void> handle_transient_verify2(std::span<const std::byte, 64> client_signature);
    [[nodiscard]] std::array<std::byte, 32> ed25519_public_key() const;
    fairplay_pairing(fairplay_pairing&&) noexcept;
    fairplay_pairing& operator=(fairplay_pairing&&) noexcept;
    ~fairplay_pairing();

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};
class srp6a {
public:
    static result<srp6a> create_server(std::string_view username, std::string_view password);
    [[nodiscard]] std::span<const std::byte> get_public_key() const;
    [[nodiscard]] std::span<const std::byte> get_salt() const;
    [[nodiscard]] std::span<const std::byte> get_session_key() const;
    result<void> compute_session_key(std::span<const std::byte> client_public_key);
    result<bool> verify_client_proof(std::span<const std::byte> proof);
    [[nodiscard]] std::vector<std::byte> generate_server_proof() const;
    srp6a(srp6a&&) noexcept;
    srp6a& operator=(srp6a&&) noexcept;
    ~srp6a();

private:
    struct impl;
    explicit srp6a(std::unique_ptr<impl> impl_ptr);
    std::unique_ptr<impl> impl_;
};
}  // namespace mirage::crypto
