#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/receiver_session.hpp"
#include "core/receiver_source.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"

namespace mirage::protocols {

std::vector<receiver_source_descriptor> make_receiver_source_descriptors(const config& cfg);

result<std::unique_ptr<receiver_session>> make_receiver_session(
    io::io_context& ctx, const receiver_source_descriptor& source,
    crypto::ed25519_keypair* airplay_keypair, std::string device_name, std::string mac_address);

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx,
                                                                receiver_source_descriptor source,
                                                                crypto::ed25519_keypair keypair,
                                                                std::string device_name,
                                                                std::string mac_address);

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx,
                                                             receiver_source_descriptor source,
                                                             std::string device_name);

std::unique_ptr<receiver_session> make_wfd_receiver_session(io::io_context& ctx,
                                                            receiver_source_descriptor source);

}  // namespace mirage::protocols
