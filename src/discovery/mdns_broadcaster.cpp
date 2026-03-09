#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "core/log.hpp"
#include "discovery/discovery.hpp"
namespace mirage::discovery {
namespace {
constexpr uint16_t mdns_port = 5353;
constexpr auto mdns_multicast_addr_v4 = "224.0.0.251";
constexpr uint16_t dns_flag_response = 0x8400;
constexpr uint16_t dns_type_a = 1;
constexpr uint16_t dns_type_ptr = 12;
constexpr uint16_t dns_type_txt = 16;
constexpr uint16_t dns_type_aaaa = 28;
constexpr uint16_t dns_type_srv = 33;
constexpr uint16_t dns_type_any = 255;
constexpr uint16_t dns_class_in = 1;
constexpr uint16_t dns_class_in_flush = 0x8001;
constexpr uint32_t default_ttl = 4500;
void write_u16_be(std::vector<std::byte>& buf, uint16_t val) {
    buf.push_back(static_cast<std::byte>((val >> 8) & 0xFF));
    buf.push_back(static_cast<std::byte>(val & 0xFF));
}
void write_u32_be(std::vector<std::byte>& buf, uint32_t val) {
    buf.push_back(static_cast<std::byte>((val >> 24) & 0xFF));
    buf.push_back(static_cast<std::byte>((val >> 16) & 0xFF));
    buf.push_back(static_cast<std::byte>((val >> 8) & 0xFF));
    buf.push_back(static_cast<std::byte>(val & 0xFF));
}
uint16_t read_u16_be(std::span<const std::byte> data, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                 static_cast<uint16_t>(data[offset + 1]));
}
}  // namespace
mdns_broadcaster::mdns_broadcaster(io::io_context& ctx)
    : socket_(io::udp_socket::bind(ctx, mdns_port)),
      multicast_endpoint_{io::ip_address::parse(mdns_multicast_addr_v4), mdns_port},
      local_address_(io::ip_address::v4_any()) {
    socket_.set_reuse_address(true);
    socket_.join_multicast(io::ip_address::parse(mdns_multicast_addr_v4));
    socket_.set_multicast_loopback(true);
    auto system_hostname = io::host_name();
    for (char& c : system_hostname) {
        if (c == '.') {
            c = '-';
        }
    }
    hostname_ = system_hostname + ".local";
    mirage::log::info("mDNS hostname: {}", hostname_);
    auto interfaces = enumerate_interfaces();
    if (interfaces) {
        for (const auto& iface : *interfaces) {
            if (!iface.is_loopback && iface.is_up) {
                for (const auto& addr : iface.addresses) {
                    if (addr.is_v4()) {
                        local_address_ = addr;
                        mirage::log::info("mDNS will advertise IP: {} (from {})",
                                          local_address_.to_string(), iface.name);
                        break;
                    }
                }
                if (local_address_ != io::ip_address::v4_any()) {
                    break;
                }
            }
        }
    }
    if (local_address_ == io::ip_address::v4_any()) {
        mirage::log::warn("Could not discover local IP address, mDNS may not work correctly");
    } else {
        socket_.set_multicast_interface(local_address_);
        mirage::log::debug("Set multicast outbound interface to {}", local_address_.to_string());
    }
}
result<void> mdns_broadcaster::register_service(service_record record) {
    mirage::log::info("Registering mDNS service: {} ({}:{})", record.name, record.service_type,
                      record.port);
    services_.push_back(std::move(record));
    return {};
}
void mdns_broadcaster::unregister_all() {
    services_.clear();
}
io::task<result<void>> mdns_broadcaster::announce() {
    auto a_packet = build_a_record_response();
    mirage::log::debug("Announcing hostname A record: {} -> {} ({} bytes)", hostname_,
                       local_address_.to_string(), a_packet.size());
    try {
        auto bytes_sent = co_await socket_.async_send_to(std::span<const std::byte>{a_packet},
                                                         multicast_endpoint_);
        mirage::log::debug("Sent {} bytes to {}", bytes_sent, multicast_endpoint_.to_string());
    } catch (const std::exception& e) {
        mirage::log::warn("Failed to send A record: {}", e.what());
    }
    for (const auto& service : services_) {
        auto packet = build_dns_response(service);
        mirage::log::debug("Announcing service: {} ({} bytes)", service.name, packet.size());
        try {
            auto bytes_sent = co_await socket_.async_send_to(std::span<const std::byte>{packet},
                                                             multicast_endpoint_);
            mirage::log::debug("Sent service {} ({} bytes) to multicast", service.name, bytes_sent);
        } catch (const std::exception& e) {
            co_return std::unexpected(mirage_error::network(
                std::format("failed to send mDNS announcement: {}", e.what())));
        }
    }
    co_return result<void>{};
}
io::task<void> mdns_broadcaster::run() {
    running_ = true;
    std::array<std::byte, 9000> buffer{};
    co_await announce();
    while (running_) {
        try {
            io::endpoint sender;
            auto n = co_await socket_.async_recv_from(buffer, sender);
            auto packet = std::span{buffer.data(), n};
            if (is_query_for_our_services(packet)) {
                mirage::log::debug("Received mDNS query from {}, responding",
                                   sender.addr.to_string());
                co_await announce();
            }
        } catch (const std::system_error& e) {
            if (e.code() != std::errc::operation_canceled) {
                mirage::log::warn("mDNS receive error: {}", e.what());
            }
        }
    }
}
void mdns_broadcaster::stop() {
    running_ = false;
    socket_.close();
}
std::vector<std::byte> mdns_broadcaster::encode_dns_name(std::string_view name) {
    std::vector<std::byte> encoded;
    size_t start = 0;
    while (start < name.size()) {
        auto dot = name.find('.', start);
        if (dot == std::string_view::npos) {
            dot = name.size();
        }
        auto label_len = dot - start;
        label_len = std::min<unsigned long>(label_len, 63);
        encoded.push_back(static_cast<std::byte>(label_len));
        for (size_t i = start; i < start + label_len; ++i) {
            encoded.push_back(static_cast<std::byte>(name[i]));
        }
        start = dot + 1;
    }
    encoded.push_back(std::byte{0});
    return encoded;
}
std::vector<std::byte> mdns_broadcaster::build_dns_response(const service_record& svc) {
    std::vector<std::byte> packet;
    packet.reserve(512);
    write_u16_be(packet, 0);
    write_u16_be(packet, dns_flag_response);
    write_u16_be(packet, 0);
    write_u16_be(packet, 1);
    write_u16_be(packet, 0);
    write_u16_be(packet, 3);
    auto ptr_name = svc.service_type + "." + svc.domain;
    auto encoded_ptr_name = encode_dns_name(ptr_name);
    packet.insert(packet.end(), encoded_ptr_name.begin(), encoded_ptr_name.end());
    write_u16_be(packet, dns_type_ptr);
    write_u16_be(packet, dns_class_in);
    write_u32_be(packet, default_ttl);
    auto instance_name = svc.name + "." + svc.service_type + "." + svc.domain;
    auto encoded_instance = encode_dns_name(instance_name);
    write_u16_be(packet, static_cast<uint16_t>(encoded_instance.size()));
    packet.insert(packet.end(), encoded_instance.begin(), encoded_instance.end());
    packet.insert(packet.end(), encoded_instance.begin(), encoded_instance.end());
    write_u16_be(packet, dns_type_srv);
    write_u16_be(packet, dns_class_in_flush);
    write_u32_be(packet, default_ttl);
    auto encoded_hostname = encode_dns_name(hostname_);
    uint16_t srv_rdlen = static_cast<uint16_t>(6 + encoded_hostname.size());
    write_u16_be(packet, srv_rdlen);
    write_u16_be(packet, 0);
    write_u16_be(packet, 0);
    write_u16_be(packet, svc.port);
    packet.insert(packet.end(), encoded_hostname.begin(), encoded_hostname.end());
    packet.insert(packet.end(), encoded_instance.begin(), encoded_instance.end());
    write_u16_be(packet, dns_type_txt);
    write_u16_be(packet, dns_class_in_flush);
    write_u32_be(packet, default_ttl);
    std::vector<std::byte> txt_data;
    for (const auto& [key, value] : svc.txt_records) {
        auto txt_entry = key;
        txt_entry += "=";
        txt_entry += value;
        if (txt_entry.size() > 255) {
            continue;
        }
        txt_data.push_back(static_cast<std::byte>(txt_entry.size()));
        for (char c : txt_entry) {
            txt_data.push_back(static_cast<std::byte>(c));
        }
    }
    write_u16_be(packet, static_cast<uint16_t>(txt_data.size()));
    packet.insert(packet.end(), txt_data.begin(), txt_data.end());
    packet.insert(packet.end(), encoded_hostname.begin(), encoded_hostname.end());
    write_u16_be(packet, dns_type_a);
    write_u16_be(packet, dns_class_in_flush);
    write_u32_be(packet, default_ttl);
    write_u16_be(packet, 4);
    auto bytes = local_address_.to_v4_bytes();
    for (auto b : bytes) {
        packet.push_back(static_cast<std::byte>(b));
    }
    return packet;
}
std::vector<std::byte> mdns_broadcaster::build_a_record_response() {
    std::vector<std::byte> packet;
    packet.reserve(128);
    write_u16_be(packet, 0);
    write_u16_be(packet, dns_flag_response);
    write_u16_be(packet, 0);
    write_u16_be(packet, 1);
    write_u16_be(packet, 0);
    write_u16_be(packet, 0);
    auto encoded_hostname = encode_dns_name(hostname_);
    packet.insert(packet.end(), encoded_hostname.begin(), encoded_hostname.end());
    write_u16_be(packet, dns_type_a);
    write_u16_be(packet, dns_class_in_flush);
    write_u32_be(packet, default_ttl);
    write_u16_be(packet, 4);
    auto bytes = local_address_.to_v4_bytes();
    for (auto b : bytes) {
        packet.push_back(static_cast<std::byte>(b));
    }
    return packet;
}
bool mdns_broadcaster::is_query_for_our_services(std::span<const std::byte> packet) {
    if (packet.size() < 12) {
        return false;
    }
    uint16_t flags = read_u16_be(packet, 2);
    if ((flags & 0x8000) != 0) {
        return false;
    }
    uint16_t qdcount = read_u16_be(packet, 4);
    if (qdcount == 0) {
        return false;
    }
    size_t offset = 12;
    for (uint16_t i = 0; i < qdcount && offset < packet.size(); ++i) {
        std::string qname;
        while (offset < packet.size()) {
            auto len = static_cast<uint8_t>(packet[offset]);
            if (len == 0) {
                offset++;
                break;
            }
            if ((len & 0xC0) == 0xC0) {
                offset += 2;
                break;
            }
            offset++;
            if (!qname.empty()) {
                qname += ".";
            }
            for (uint8_t j = 0; j < len && offset < packet.size(); ++j) {
                qname += static_cast<char>(packet[offset++]);
            }
        }
        if (offset + 4 > packet.size()) {
            break;
        }
        uint16_t qtype = read_u16_be(packet, offset);
        offset += 4;
        std::string qtype_str;
        switch (qtype) {
            case dns_type_a:
                qtype_str = "A";
                break;
            case dns_type_ptr:
                qtype_str = "PTR";
                break;
            case dns_type_txt:
                qtype_str = "TXT";
                break;
            case dns_type_srv:
                qtype_str = "SRV";
                break;
            case dns_type_aaaa:
                qtype_str = "AAAA";
                break;
            case dns_type_any:
                qtype_str = "ANY";
                break;
            default:
                qtype_str = std::to_string(qtype);
                break;
        }
        mirage::log::debug("mDNS query for: {} (type {} = {})", qname, qtype, qtype_str);
        if (qname == hostname_) {
            if (qtype == dns_type_a || qtype == dns_type_any || qtype == dns_type_aaaa) {
                mirage::log::info("Query matches our hostname: {} (responding with A record)",
                                  hostname_);
                return true;
            }
        }
        for (const auto& svc : services_) {
            auto service_fqdn = svc.service_type + "." + svc.domain;
            auto instance_fqdn = svc.name + "." + service_fqdn;
            if (qname == service_fqdn || qname == instance_fqdn) {
                mirage::log::info("Query matches service: {}", qname);
                return true;
            }
        }
    }
    return false;
}
service_record create_airplay_service(std::string_view name, uint16_t port,
                                      std::span<const std::byte, 32> ed25519_pubkey,
                                      std::string_view mac_address) {
    return {.name = std::string(name),
            .service_type = "_airplay._tcp",
            .domain = "local",
            .port = port,
            .txt_records = {
                {"deviceid", std::string(mac_address)},
                {"features", "0x5A7FFEE6,0x0"},
                {"flags", "0x4"},
                {"model", "AppleTV3,2"},
                {"pk", base64_encode(ed25519_pubkey)},
                {"pi", generate_uuid()},
                {"srcvers", "220.68"},
                {"vv", "2"},
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
                {"am", "AppleTV3,2"},
                {"ch", "2"},
                {"cn", "0,1,2,3"},
                {"da", "true"},
                {"et", "0,3,5"},
                {"ft", "0x5A7FFEE6,0x0"},
                {"md", "0,1,2"},
                {"pw", "false"},
                {"sf", "0x4"},
                {"sr", "44100"},
                {"ss", "16"},
                {"sv", "false"},
                {"tp", "UDP"},
                {"vn", "65537"},
                {"vs", "220.68"},
                {"vv", "2"},
            }};
}
service_record create_cast_service(std::string_view name, uint16_t port, std::string_view uuid) {
    return {.name = std::string(name),
            .service_type = "_googlecast._tcp",
            .domain = "local",
            .port = port,
            .txt_records = {
                {"id", std::string(uuid)},
                {"cd", generate_uuid().substr(0, 8)},
                {"rm", ""},
                {"ve", "05"},
                {"md", "Mirage"},
                {"ic", "/setup/icon.png"},
                {"fn", std::string(name)},
                {"ca", "4101"},
                {"st", "0"},
                {"bs", "FA8FCA7D3E16"},
                {"nf", "1"},
                {"rs", ""},
            }};
}
}  // namespace mirage::discovery
