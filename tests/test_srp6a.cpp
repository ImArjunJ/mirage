#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <openssl/bn.h>
#include <openssl/sha.h>

#include "crypto/crypto.hpp"

namespace {
using bn_ptr = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using ctx_ptr = std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)>;

constexpr const char* kSrpPrime =
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
    "43DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

std::vector<unsigned char> bn_to_bytes(const BIGNUM* bn) {
    const int len = BN_num_bytes(bn);
    std::vector<unsigned char> bytes(static_cast<size_t>(len));
    if (len > 0) {
        BN_bn2bin(bn, bytes.data());
    }
    return bytes;
}

std::vector<unsigned char> bn_to_padded_bytes(const BIGNUM* bn, size_t pad_len) {
    std::vector<unsigned char> bytes(pad_len, 0);
    const int bn_len = BN_num_bytes(bn);
    if (bn_len > 0 && std::cmp_less_equal(bn_len, pad_len)) {
        BN_bn2bin(bn, bytes.data() + (pad_len - static_cast<size_t>(bn_len)));
    }
    return bytes;
}

void append_bytes(std::vector<unsigned char>& out, std::span<const std::byte> bytes) {
    out.insert(out.end(), reinterpret_cast<const unsigned char*>(bytes.data()),
               reinterpret_cast<const unsigned char*>(bytes.data()) + bytes.size());
}

std::vector<std::byte> to_byte_vector(std::span<const unsigned char> bytes) {
    std::vector<std::byte> out(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        out[i] = static_cast<std::byte>(bytes[i]);
    }
    return out;
}

bool compute_client_values(const mirage::crypto::srp6a& server, std::vector<std::byte>& client_public,
                           std::vector<std::byte>& client_proof) {
    BIGNUM* raw_n = nullptr;
    if (BN_hex2bn(&raw_n, kSrpPrime) == 0) {
        return false;
    }
    bn_ptr n(raw_n, BN_free);
    bn_ptr g(BN_new(), BN_free);
    bn_ptr a(BN_new(), BN_free);
    bn_ptr a_pub(BN_new(), BN_free);
    bn_ptr b_pub(BN_bin2bn(reinterpret_cast<const unsigned char*>(server.get_public_key().data()),
                           static_cast<int>(server.get_public_key().size()), nullptr),
                 BN_free);
    ctx_ptr ctx(BN_CTX_new(), BN_CTX_free);
    if (!g || !a || !a_pub || !b_pub || !ctx || BN_set_word(g.get(), 5) != 1 ||
        BN_set_word(a.get(), 37) != 1) {
        return false;
    }

    const size_t n_len = static_cast<size_t>(BN_num_bytes(n.get()));
    if (BN_mod_exp(a_pub.get(), g.get(), a.get(), n.get(), ctx.get()) != 1) {
        return false;
    }
    client_public = to_byte_vector(bn_to_padded_bytes(a_pub.get(), n_len));

    const auto n_padded = bn_to_padded_bytes(n.get(), n_len);
    const auto g_padded = bn_to_padded_bytes(g.get(), n_len);
    std::vector<unsigned char> k_input;
    k_input.insert(k_input.end(), n_padded.begin(), n_padded.end());
    k_input.insert(k_input.end(), g_padded.begin(), g_padded.end());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> k_hash{};
    SHA512(k_input.data(), k_input.size(), k_hash.data());
    bn_ptr k(BN_bin2bn(k_hash.data(), SHA512_DIGEST_LENGTH, nullptr), BN_free);

    const std::string credentials = "Pair-Setup:3939";
    std::array<unsigned char, SHA512_DIGEST_LENGTH> inner_hash{};
    SHA512(reinterpret_cast<const unsigned char*>(credentials.data()), credentials.size(),
           inner_hash.data());
    std::vector<unsigned char> x_input;
    append_bytes(x_input, server.get_salt());
    x_input.insert(x_input.end(), inner_hash.begin(), inner_hash.end());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> x_hash{};
    SHA512(x_input.data(), x_input.size(), x_hash.data());
    bn_ptr x(BN_bin2bn(x_hash.data(), SHA512_DIGEST_LENGTH, nullptr), BN_free);

    std::vector<unsigned char> u_input;
    append_bytes(u_input, client_public);
    append_bytes(u_input, server.get_public_key());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> u_hash{};
    SHA512(u_input.data(), u_input.size(), u_hash.data());
    bn_ptr u(BN_bin2bn(u_hash.data(), SHA512_DIGEST_LENGTH, nullptr), BN_free);

    bn_ptr gx(BN_new(), BN_free);
    bn_ptr kgx(BN_new(), BN_free);
    bn_ptr base(BN_new(), BN_free);
    bn_ptr ux(BN_new(), BN_free);
    bn_ptr exponent(BN_new(), BN_free);
    bn_ptr s(BN_new(), BN_free);
    if (!k || !x || !u || !gx || !kgx || !base || !ux || !exponent || !s ||
        BN_mod_exp(gx.get(), g.get(), x.get(), n.get(), ctx.get()) != 1 ||
        BN_mod_mul(kgx.get(), k.get(), gx.get(), n.get(), ctx.get()) != 1 ||
        BN_mod_sub(base.get(), b_pub.get(), kgx.get(), n.get(), ctx.get()) != 1 ||
        BN_mul(ux.get(), u.get(), x.get(), ctx.get()) != 1 ||
        BN_add(exponent.get(), a.get(), ux.get()) != 1 ||
        BN_mod_exp(s.get(), base.get(), exponent.get(), n.get(), ctx.get()) != 1) {
        return false;
    }

    const auto s_bytes = bn_to_bytes(s.get());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> session_key{};
    SHA512(s_bytes.data(), s_bytes.size(), session_key.data());

    std::array<unsigned char, SHA512_DIGEST_LENGTH> n_hash{};
    std::array<unsigned char, SHA512_DIGEST_LENGTH> g_hash{};
    std::array<unsigned char, SHA512_DIGEST_LENGTH> user_hash{};
    SHA512(n_padded.data(), n_padded.size(), n_hash.data());
    SHA512(g_padded.data(), g_padded.size(), g_hash.data());
    const std::string username = "Pair-Setup";
    SHA512(reinterpret_cast<const unsigned char*>(username.data()), username.size(), user_hash.data());
    for (size_t i = 0; i < n_hash.size(); ++i) {
        n_hash[i] ^= g_hash[i];
    }

    std::vector<unsigned char> m1_input;
    m1_input.insert(m1_input.end(), n_hash.begin(), n_hash.end());
    m1_input.insert(m1_input.end(), user_hash.begin(), user_hash.end());
    append_bytes(m1_input, server.get_salt());
    append_bytes(m1_input, client_public);
    append_bytes(m1_input, server.get_public_key());
    m1_input.insert(m1_input.end(), session_key.begin(), session_key.end());
    std::array<unsigned char, SHA512_DIGEST_LENGTH> proof{};
    SHA512(m1_input.data(), m1_input.size(), proof.data());
    client_proof = to_byte_vector(proof);
    return true;
}
}  // namespace

int main() {
    auto server_result = mirage::crypto::srp6a::create_server("Pair-Setup", "3939");
    if (!server_result) {
        std::cerr << server_result.error().format() << '\n';
        return 1;
    }
    auto server = std::move(*server_result);

    std::vector<std::byte> client_public;
    std::vector<std::byte> client_proof;
    if (!compute_client_values(server, client_public, client_proof)) {
        std::cerr << "failed to compute SRP client fixture\n";
        return 1;
    }

    auto compute_result = server.compute_session_key(client_public);
    if (!compute_result) {
        std::cerr << compute_result.error().format() << '\n';
        return 1;
    }

    auto bad_proof = client_proof;
    bad_proof[0] ^= std::byte{0x01};
    auto bad_verify = server.verify_client_proof(bad_proof);
    if (!bad_verify || *bad_verify) {
        std::cerr << "bad SRP proof was accepted\n";
        return 1;
    }

    auto good_verify = server.verify_client_proof(client_proof);
    if (!good_verify || !*good_verify) {
        std::cerr << "valid SRP proof was rejected\n";
        return 1;
    }

    auto invalid_server_result = mirage::crypto::srp6a::create_server("Pair-Setup", "3939");
    if (!invalid_server_result) {
        std::cerr << invalid_server_result.error().format() << '\n';
        return 1;
    }
    auto invalid_server = std::move(*invalid_server_result);
    std::vector<std::byte> zero_public(client_public.size(), std::byte{0});
    if (invalid_server.compute_session_key(zero_public)) {
        std::cerr << "zero SRP public key was accepted\n";
        return 1;
    }

    return 0;
}
