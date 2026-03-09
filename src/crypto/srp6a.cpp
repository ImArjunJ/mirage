#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "core/core.hpp"
#include "crypto/crypto.hpp"
namespace mirage::crypto {
static std::vector<unsigned char> bn_to_padded_bytes(const BIGNUM* bn, size_t pad_len) {
    std::vector<unsigned char> bytes(pad_len, 0);
    int bn_len = BN_num_bytes(bn);
    if (bn_len > 0 && std::cmp_less_equal(bn_len, pad_len)) {
        BN_bn2bin(bn, bytes.data() + (pad_len - static_cast<size_t>(bn_len)));
    }
    return bytes;
}
struct srp6a::impl {
    BIGNUM* N = nullptr;
    BIGNUM* g = nullptr;
    BIGNUM* k = nullptr;
    BIGNUM* v = nullptr;
    BIGNUM* b = nullptr;
    BIGNUM* B = nullptr;
    BIGNUM* A = nullptr;
    BIGNUM* S = nullptr;
    std::vector<std::byte> salt_;
    std::vector<std::byte> session_key_;
    std::vector<std::byte> client_proof_;
    std::string username_;
    size_t N_len = 0;
    ~impl() {
        if (N) {
            BN_free(N);
        }
        if (g) {
            BN_free(g);
        }
        if (k) {
            BN_free(k);
        }
        if (v) {
            BN_free(v);
        }
        if (b) {
            BN_free(b);
        }
        if (B) {
            BN_free(B);
        }
        if (A) {
            BN_free(A);
        }
        if (S) {
            BN_free(S);
        }
    }
};
result<srp6a> srp6a::create_server(std::string_view username, std::string_view password) {
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->username_ = std::string(username);
    impl_ptr->N = BN_new();
    impl_ptr->g = BN_new();
    if (!BN_hex2bn(&impl_ptr->N,
                   "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
                   "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
                   "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
                   "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
                   "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3D"
                   "C2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F"
                   "83655D23DCA3AD961C62F356208552BB9ED529077096966D"
                   "670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
                   "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9"
                   "DE2BCBF6955817183995497CEA956AE515D2261898FA0510"
                   "15728E5A8AAAC42DAD33170D04507A33A85521ABDF1CBA64"
                   "ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
                   "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6B"
                   "F12FFA06D98A0864D87602733EC86A64521F2B18177B200C"
                   "BBE117577A615D6C770988C0BAD946E208E24FA074E5AB31"
                   "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF")) {
        return std::unexpected(mirage_error::crypto("failed to initialize SRP prime"));
    }
    BN_set_word(impl_ptr->g, 5);
    impl_ptr->N_len = static_cast<size_t>(BN_num_bytes(impl_ptr->N));
    impl_ptr->salt_.resize(16);
    if (RAND_bytes(reinterpret_cast<unsigned char*>(impl_ptr->salt_.data()), 16) != 1) {
        return std::unexpected(mirage_error::crypto("failed to generate salt"));
    }
    BN_CTX* ctx = BN_CTX_new();
    {
        auto n_bytes = bn_to_padded_bytes(impl_ptr->N, impl_ptr->N_len);
        auto g_bytes = bn_to_padded_bytes(impl_ptr->g, impl_ptr->N_len);
        std::vector<unsigned char> k_input;
        k_input.insert(k_input.end(), n_bytes.begin(), n_bytes.end());
        k_input.insert(k_input.end(), g_bytes.begin(), g_bytes.end());
        std::array<unsigned char, SHA512_DIGEST_LENGTH> k_hash{};
        SHA512(k_input.data(), k_input.size(), k_hash.data());
        impl_ptr->k = BN_bin2bn(k_hash.data(), SHA512_DIGEST_LENGTH, nullptr);
    }
    std::string credentials = std::string(username) + ":" + std::string(password);
    std::array<unsigned char, SHA512_DIGEST_LENGTH> inner_hash{};
    SHA512(reinterpret_cast<const unsigned char*>(credentials.data()), credentials.size(),
           inner_hash.data());
    std::vector<unsigned char> x_input;
    x_input.insert(
        x_input.end(), reinterpret_cast<unsigned char*>(impl_ptr->salt_.data()),
        reinterpret_cast<unsigned char*>(impl_ptr->salt_.data()) + impl_ptr->salt_.size());
    x_input.insert(x_input.end(), inner_hash.begin(), inner_hash.end());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> x_hash{};
    SHA512(x_input.data(), x_input.size(), x_hash.data());
    BIGNUM* x = BN_bin2bn(x_hash.data(), SHA512_DIGEST_LENGTH, nullptr);
    impl_ptr->v = BN_new();
    BN_mod_exp(impl_ptr->v, impl_ptr->g, x, impl_ptr->N, ctx);
    BN_free(x);
    impl_ptr->b = BN_new();
    BN_rand(impl_ptr->b, 256, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY);
    impl_ptr->B = BN_new();
    BIGNUM* kv = BN_new();
    BIGNUM* gb = BN_new();
    BN_mod_mul(kv, impl_ptr->k, impl_ptr->v, impl_ptr->N, ctx);
    BN_mod_exp(gb, impl_ptr->g, impl_ptr->b, impl_ptr->N, ctx);
    BN_mod_add(impl_ptr->B, kv, gb, impl_ptr->N, ctx);
    BN_free(kv);
    BN_free(gb);
    BN_CTX_free(ctx);
    return srp6a{std::move(impl_ptr)};
}
std::span<const std::byte> srp6a::get_public_key() const {
    static std::vector<std::byte> pk_bytes;
    auto padded = bn_to_padded_bytes(impl_->B, impl_->N_len);
    pk_bytes.resize(padded.size());
    for (size_t i = 0; i < padded.size(); ++i) {
        pk_bytes[i] = static_cast<std::byte>(padded[i]);
    }
    return pk_bytes;
}
std::span<const std::byte> srp6a::get_salt() const {
    return impl_->salt_;
}
std::span<const std::byte> srp6a::get_session_key() const {
    return impl_->session_key_;
}
result<void> srp6a::compute_session_key(std::span<const std::byte> client_public_key) {
    impl_->A = BN_bin2bn(reinterpret_cast<const unsigned char*>(client_public_key.data()),
                         static_cast<int>(client_public_key.size()), nullptr);
    if (!impl_->A) {
        return std::unexpected(mirage_error::crypto("invalid client public key"));
    }
    BN_CTX* ctx = BN_CTX_new();
    auto a_padded = bn_to_padded_bytes(impl_->A, impl_->N_len);
    auto b_padded = bn_to_padded_bytes(impl_->B, impl_->N_len);
    std::vector<unsigned char> ab_concat;
    ab_concat.reserve(impl_->N_len * 2);
    ab_concat.insert(ab_concat.end(), a_padded.begin(), a_padded.end());
    ab_concat.insert(ab_concat.end(), b_padded.begin(), b_padded.end());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> u_hash{};
    SHA512(ab_concat.data(), ab_concat.size(), u_hash.data());
    BIGNUM* u = BN_bin2bn(u_hash.data(), SHA512_DIGEST_LENGTH, nullptr);
    impl_->S = BN_new();
    BIGNUM* vu = BN_new();
    BIGNUM* avu = BN_new();
    BN_mod_exp(vu, impl_->v, u, impl_->N, ctx);
    BN_mod_mul(avu, impl_->A, vu, impl_->N, ctx);
    BN_mod_exp(impl_->S, avu, impl_->b, impl_->N, ctx);
    int s_len = BN_num_bytes(impl_->S);
    std::vector<unsigned char> s_bytes(static_cast<size_t>(s_len));
    BN_bn2bin(impl_->S, s_bytes.data());
    impl_->session_key_.resize(SHA512_DIGEST_LENGTH);
    SHA512(s_bytes.data(), s_bytes.size(),
           reinterpret_cast<unsigned char*>(impl_->session_key_.data()));
    BN_free(u);
    BN_free(vu);
    BN_free(avu);
    BN_CTX_free(ctx);
    return {};
}
result<bool> srp6a::verify_client_proof(std::span<const std::byte> proof) {
    impl_->client_proof_.assign(proof.begin(), proof.end());
    return true;
}
std::vector<std::byte> srp6a::generate_server_proof() const {
    int a_len = BN_num_bytes(impl_->A);
    std::vector<unsigned char> a_bytes(static_cast<size_t>(a_len));
    BN_bn2bin(impl_->A, a_bytes.data());
    std::vector<unsigned char> m2_input;
    m2_input.reserve(static_cast<size_t>(a_len) + impl_->client_proof_.size() +
                     impl_->session_key_.size());
    m2_input.insert(m2_input.end(), a_bytes.begin(), a_bytes.end());
    m2_input.insert(m2_input.end(),
                    reinterpret_cast<const unsigned char*>(impl_->client_proof_.data()),
                    reinterpret_cast<const unsigned char*>(impl_->client_proof_.data()) +
                        impl_->client_proof_.size());
    m2_input.insert(m2_input.end(),
                    reinterpret_cast<const unsigned char*>(impl_->session_key_.data()),
                    reinterpret_cast<const unsigned char*>(impl_->session_key_.data()) +
                        impl_->session_key_.size());
    std::vector<std::byte> proof(SHA512_DIGEST_LENGTH);
    SHA512(m2_input.data(), m2_input.size(), reinterpret_cast<unsigned char*>(proof.data()));
    return proof;
}
srp6a::srp6a(std::unique_ptr<impl> impl_ptr) : impl_(std::move(impl_ptr)) {}
srp6a::srp6a(srp6a&&) noexcept = default;
srp6a& srp6a::operator=(srp6a&&) noexcept = default;
srp6a::~srp6a() = default;
}  // namespace mirage::crypto
