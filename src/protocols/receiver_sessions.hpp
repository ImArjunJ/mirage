#pragma once

#include <memory>
#include <string>

#include "core/receiver_identity.hpp"
#include "core/receiver_session.hpp"
#include "core/receiver_source.hpp"
#include "crypto/crypto.hpp"
#include "io/io.hpp"

namespace mirage::protocols {

std::unique_ptr<receiver_session> make_airplay_receiver_session(io::io_context& ctx,
                                                                receiver_source_descriptor source,
                                                                crypto::ed25519_keypair keypair,
                                                                std::string device_name,
                                                                std::string mac_address,
                                                                receiver_session_observer* observer);

std::unique_ptr<receiver_session> make_cast_receiver_session(io::io_context& ctx,
                                                             receiver_source_descriptor source,
                                                             std::string device_name,
                                                             protocol_receiver_identity identity,
                                                             receiver_session_observer* observer);

std::unique_ptr<receiver_session> make_wfd_receiver_session(io::io_context& ctx,
                                                            receiver_source_descriptor source,
                                                            receiver_session_observer* observer);

}  // namespace mirage::protocols
