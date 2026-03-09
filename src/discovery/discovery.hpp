#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/core.hpp"
#include "io/io.hpp"
namespace mirage::discovery {
struct service_record {
    std::string name;
    std::string service_type;
    std::string domain;
    uint16_t port;
    std::vector<std::pair<std::string, std::string>> txt_records;
    [[nodiscard]] std::string full_name() const { return name + "." + service_type + "." + domain; }
};
struct network_interface {
    std::string name;
    std::string mac_address;
    std::vector<io::ip_address> addresses;
    bool is_up = false;
    bool is_loopback = false;
};
result<std::vector<network_interface>> enumerate_interfaces();
result<std::string> get_mac_address(std::string_view interface_name);
class mdns_broadcaster {
public:
    explicit mdns_broadcaster(io::io_context& ctx);
    result<void> register_service(service_record record);
    void unregister_all();
    io::task<result<void>> announce();
    io::task<void> run();
    void stop();

private:
    std::vector<std::byte> build_dns_response(const service_record& svc);
    std::vector<std::byte> build_ptr_record(const service_record& svc);
    std::vector<std::byte> build_srv_record(const service_record& svc);
    std::vector<std::byte> build_txt_record(const service_record& svc);
    std::vector<std::byte> build_a_record(const service_record& svc);
    std::vector<std::byte> build_a_record_response();
    bool is_query_for_our_services(std::span<const std::byte> packet);
    static std::vector<std::byte> encode_dns_name(std::string_view name);
    io::udp_socket socket_;
    io::endpoint multicast_endpoint_;
    std::vector<service_record> services_;
    bool running_ = false;
    std::string hostname_;
    io::ip_address local_address_;
};
service_record create_airplay_service(std::string_view name, uint16_t port,
                                      std::span<const std::byte, 32> ed25519_pubkey,
                                      std::string_view mac_address);
service_record create_raop_service(std::string_view name, uint16_t port,
                                   std::string_view mac_address);
service_record create_cast_service(std::string_view name, uint16_t port, std::string_view uuid);
}  // namespace mirage::discovery
