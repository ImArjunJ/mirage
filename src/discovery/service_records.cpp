#include "discovery/discovery.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "core/core.hpp"
#include "core/receiver_identity.hpp"
#include "protocols/airplay_protocol.hpp"

namespace mirage::discovery {

service_record create_airplay_service(std::string_view name, uint16_t port,
                                      std::span<const std::byte, 32> ed25519_pubkey,
                                      std::string_view mac_address) {
    auto identity = derive_protocol_identity(ed25519_pubkey, "airplay");
    return {.name = std::string(name),
            .service_type = "_airplay._tcp",
            .domain = "local",
            .port = port,
            .txt_records = {
                {"deviceid", std::string(mac_address)},
                {"features", protocols::airplay::feature_txt()},
                {"flags", "0x4"},
                {"model", std::string(protocols::airplay::compatibility_model)},
                {"pk", base64_encode(ed25519_pubkey)},
                {"pi", identity.uuid},
                {"srcvers", std::string(protocols::airplay::source_version)},
                {"vv", std::to_string(protocols::airplay::protocol_version)},
            }};
}

service_record create_raop_service(std::string_view name, uint16_t port,
                                   std::string_view mac_address) {
    std::string mac_clean;
    for (char c : mac_address) {
        if (c != ':') {
            mac_clean += c;
        }
    }
    return {.name = mac_clean + "@" + std::string(name),
            .service_type = "_raop._tcp",
            .domain = "local",
            .port = port,
            .txt_records = {
                {"am", std::string(protocols::airplay::compatibility_model)},
                {"ch", std::to_string(protocols::airplay::default_channels)},
                {"cn", "0,1,2,3"},
                {"da", "true"},
                {"et", "0,3,5"},
                {"ft", protocols::airplay::feature_txt()},
                {"md", "0,1,2"},
                {"pw", "false"},
                {"sf", "0x4"},
                {"sr", std::to_string(protocols::airplay::default_sample_rate)},
                {"ss", "16"},
                {"sv", "false"},
                {"tp", "UDP"},
                {"vn", "65537"},
                {"vs", std::string(protocols::airplay::source_version)},
                {"vv", std::to_string(protocols::airplay::protocol_version)},
            }};
}

service_record create_cast_service(std::string_view name, uint16_t port,
                                   const protocol_receiver_identity& identity) {
    return {.name = std::string(name),
            .service_type = "_googlecast._tcp",
            .domain = "local",
            .port = port,
            .txt_records = {
                {"id", identity.uuid},
                {"cd", identity.short_id},
                {"rm", ""},
                {"ve", "05"},
                {"md", "Mirage"},
                {"ic", "/setup/icon.png"},
                {"fn", std::string(name)},
                {"ca", "4101"},
                {"st", "0"},
                {"bs", identity.bootstrap_id},
                {"nf", "1"},
                {"rs", ""},
            }};
}

}  // namespace mirage::discovery
