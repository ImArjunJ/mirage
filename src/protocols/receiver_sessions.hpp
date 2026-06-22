#pragma once

#include <memory>

#include "core/receiver_session.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"

namespace mirage::protocols {

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx, uint16_t port,
                                                                crypto::ed25519_keypair keypair);

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx, uint16_t port);

std::unique_ptr<receiver_session> make_wfd_receiver_session(io::io_context& ctx);

}  // namespace mirage::protocols
