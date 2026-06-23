#include "protocols/cast/tls.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <format>
#include <string>
#include <utility>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace mirage::protocols::cast {
namespace {

template <typename T, void (*FreeFn)(T*)>
struct openssl_deleter {
    void operator()(T* value) const {
        if (value != nullptr) {
            FreeFn(value);
        }
    }
};

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, openssl_deleter<EVP_PKEY, EVP_PKEY_free>>;
using evp_pkey_ctx_ptr =
    std::unique_ptr<EVP_PKEY_CTX, openssl_deleter<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using x509_ptr = std::unique_ptr<X509, openssl_deleter<X509, X509_free>>;

std::string openssl_error(std::string_view operation) {
    std::string message = std::format("{} failed", operation);
    const auto code = ERR_get_error();
    if (code != 0) {
        std::array<char, 256> buffer{};
        ERR_error_string_n(code, buffer.data(), buffer.size());
        message += ": ";
        message += buffer.data();
    }
    return message;
}

result<evp_pkey_ptr> make_private_key() {
    evp_pkey_ctx_ptr ctx{EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("initializing Cast TLS key")));
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw_key) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("generating Cast TLS key")));
    }
    return evp_pkey_ptr{raw_key};
}

result<x509_ptr> make_certificate(EVP_PKEY* key) {
    x509_ptr cert{X509_new()};
    if (!cert) {
        return std::unexpected(mirage_error::crypto(openssl_error("allocating Cast TLS cert")));
    }

    if (ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) <= 0 ||
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L * 24L * 365L) == nullptr ||
        X509_set_version(cert.get(), 2) <= 0 || X509_set_pubkey(cert.get(), key) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("configuring Cast TLS cert")));
    }

    X509_NAME* name = X509_get_subject_name(cert.get());
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("Mirage Cast Receiver"),
                                   -1, -1, 0) <= 0 ||
        X509_set_issuer_name(cert.get(), name) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("naming Cast TLS cert")));
    }

    if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("signing Cast TLS cert")));
    }

    return cert;
}

result<std::unique_ptr<ssl_ctx_st, tls_channel::ssl_ctx_deleter>> make_server_context() {
    auto key = make_private_key();
    if (!key) {
        return std::unexpected(key.error());
    }
    auto cert = make_certificate(key->get());
    if (!cert) {
        return std::unexpected(cert.error());
    }

    std::unique_ptr<ssl_ctx_st, tls_channel::ssl_ctx_deleter> ctx{
        SSL_CTX_new(TLS_server_method())};
    if (!ctx) {
        return std::unexpected(mirage_error::crypto(openssl_error("creating Cast TLS context")));
    }

    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_COMPRESSION);

    if (SSL_CTX_use_certificate(ctx.get(), cert->get()) <= 0 ||
        SSL_CTX_use_PrivateKey(ctx.get(), key->get()) <= 0 ||
        SSL_CTX_check_private_key(ctx.get()) <= 0) {
        return std::unexpected(mirage_error::crypto(openssl_error("installing Cast TLS cert")));
    }

    return ctx;
}

result<std::unique_ptr<ssl_st, tls_channel::ssl_deleter>> make_ssl(SSL_CTX* ctx) {
    std::unique_ptr<ssl_st, tls_channel::ssl_deleter> ssl{SSL_new(ctx)};
    if (!ssl) {
        return std::unexpected(mirage_error::crypto(openssl_error("creating Cast TLS session")));
    }

    BIO* rbio = BIO_new(BIO_s_mem());
    BIO* wbio = BIO_new(BIO_s_mem());
    if (rbio == nullptr || wbio == nullptr) {
        BIO_free(rbio);
        BIO_free(wbio);
        return std::unexpected(mirage_error::crypto(openssl_error("creating Cast TLS BIOs")));
    }
    BIO_set_mem_eof_return(rbio, -1);
    SSL_set_bio(ssl.get(), rbio, wbio);
    SSL_set_accept_state(ssl.get());
    return ssl;
}

mirage_error ssl_operation_error(SSL* ssl, int rc, std::string_view operation) {
    const auto ssl_error = SSL_get_error(ssl, rc);
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
        return mirage_error::network("Cast TLS peer closed connection");
    }
    return mirage_error::crypto(
        std::format("{} failed with ssl error {}", operation, ssl_error));
}

}  // namespace

void tls_channel::ssl_ctx_deleter::operator()(ssl_ctx_st* ctx) const {
    SSL_CTX_free(ctx);
}

void tls_channel::ssl_deleter::operator()(ssl_st* ssl) const {
    SSL_free(ssl);
}

tls_channel::tls_channel(io::tcp_stream socket,
                         std::unique_ptr<ssl_ctx_st, ssl_ctx_deleter> ctx,
                         std::unique_ptr<ssl_st, ssl_deleter> ssl)
    : socket_(std::move(socket)), ctx_(std::move(ctx)), ssl_(std::move(ssl)) {}

tls_channel::tls_channel(tls_channel&&) noexcept = default;

tls_channel& tls_channel::operator=(tls_channel&&) noexcept = default;

tls_channel::~tls_channel() = default;

io::task<result<tls_channel>> tls_channel::accept(io::tcp_stream socket,
                                                  std::span<const std::byte> first_packet) {
    auto ctx = make_server_context();
    if (!ctx) {
        co_return std::unexpected(ctx.error());
    }
    auto ssl = make_ssl(ctx->get());
    if (!ssl) {
        co_return std::unexpected(ssl.error());
    }

    tls_channel channel{std::move(socket), std::move(*ctx), std::move(*ssl)};
    if (auto fed = channel.feed_encrypted(first_packet); !fed) {
        co_return std::unexpected(fed.error());
    }
    auto accepted = co_await channel.handshake();
    if (!accepted) {
        co_return std::unexpected(accepted.error());
    }
    co_return std::move(channel);
}

result<void> tls_channel::feed_encrypted(std::span<const std::byte> data) {
    BIO* rbio = SSL_get_rbio(ssl_.get());
    while (!data.empty()) {
        const auto chunk_size = std::min<size_t>(data.size(), static_cast<size_t>(INT_MAX));
        const int written = BIO_write(rbio, data.data(), static_cast<int>(chunk_size));
        if (written <= 0) {
            return std::unexpected(mirage_error::crypto(openssl_error("feeding Cast TLS data")));
        }
        data = data.subspan(static_cast<size_t>(written));
    }
    return {};
}

io::task<result<void>> tls_channel::flush_pending() {
    BIO* wbio = SSL_get_wbio(ssl_.get());
    std::array<std::byte, 4096> buffer{};
    while (BIO_pending(wbio) > 0) {
        const int n = BIO_read(wbio, buffer.data(), static_cast<int>(buffer.size()));
        if (n <= 0) {
            co_return std::unexpected(
                mirage_error::crypto(openssl_error("reading Cast TLS output")));
        }
        co_await socket_.async_write(std::span<const std::byte>(buffer.data(),
                                                                static_cast<size_t>(n)));
    }
    co_return {};
}

io::task<result<void>> tls_channel::read_encrypted_packet() {
    std::array<std::byte, 4096> buffer{};
    const auto n = co_await socket_.async_read(buffer);
    co_return feed_encrypted(std::span<const std::byte>(buffer.data(), n));
}

io::task<result<void>> tls_channel::handshake() {
    while (true) {
        const int rc = SSL_do_handshake(ssl_.get());
        if (rc == 1) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            co_return {};
        }

        const auto ssl_error = SSL_get_error(ssl_.get(), rc);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            auto read = co_await read_encrypted_packet();
            if (!read) {
                co_return std::unexpected(read.error());
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            continue;
        }

        co_return std::unexpected(ssl_operation_error(ssl_.get(), rc, "Cast TLS handshake"));
    }
}

io::task<result<size_t>> tls_channel::async_read(std::span<std::byte> data) {
    while (true) {
        size_t read = 0;
        const int rc = SSL_read_ex(ssl_.get(), data.data(), data.size(), &read);
        if (rc == 1) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            co_return read;
        }

        const auto ssl_error = SSL_get_error(ssl_.get(), rc);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            auto encrypted = co_await read_encrypted_packet();
            if (!encrypted) {
                co_return std::unexpected(encrypted.error());
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            continue;
        }

        co_return std::unexpected(ssl_operation_error(ssl_.get(), rc, "Cast TLS read"));
    }
}

io::task<result<void>> tls_channel::async_write(std::span<const std::byte> data) {
    while (!data.empty()) {
        size_t written = 0;
        const int rc = SSL_write_ex(ssl_.get(), data.data(), data.size(), &written);
        if (rc == 1) {
            data = data.subspan(written);
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            continue;
        }

        const auto ssl_error = SSL_get_error(ssl_.get(), rc);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            auto encrypted = co_await read_encrypted_packet();
            if (!encrypted) {
                co_return std::unexpected(encrypted.error());
            }
            continue;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            auto flushed = co_await flush_pending();
            if (!flushed) {
                co_return std::unexpected(flushed.error());
            }
            continue;
        }

        co_return std::unexpected(ssl_operation_error(ssl_.get(), rc, "Cast TLS write"));
    }

    auto flushed = co_await flush_pending();
    if (!flushed) {
        co_return std::unexpected(flushed.error());
    }
    co_return {};
}

bool tls_channel::is_open() const {
    return socket_.is_open();
}

void tls_channel::close() {
    if (socket_.is_open()) {
        static_cast<void>(SSL_shutdown(ssl_.get()));
        socket_.close();
    }
}

}  // namespace mirage::protocols::cast
