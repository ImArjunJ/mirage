#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/core.hpp"
#include "discovery/discovery.hpp"
#ifdef __linux__
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif
namespace mirage::discovery {
#ifdef __linux__
result<std::vector<network_interface>> enumerate_interfaces() {
    std::vector<network_interface> interfaces;
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return std::unexpected(mirage_error::network("failed to enumerate interfaces"));
    }
    for (ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == nullptr) {
            continue;
        }
        std::string name = ifa->ifa_name;
        auto it = std::find_if(interfaces.begin(), interfaces.end(),
                               [&name](const auto& iface) { return iface.name == name; });
        network_interface* iface = nullptr;
        if (it == interfaces.end()) {
            interfaces.push_back({
                .name = name,
                .mac_address = {},
                .addresses = {},
                .is_up = (ifa->ifa_flags & IFF_UP) != 0,
                .is_loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0,
            });
            iface = &interfaces.back();
            if (auto mac = get_mac_address(name); mac.has_value()) {
                iface->mac_address = *mac;
            }
        } else {
            iface = &(*it);
        }
        if (ifa->ifa_addr != nullptr) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
                iface->addresses.emplace_back(io::ip_address::v4(ntohl(sin->sin_addr.s_addr)));
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(ifa->ifa_addr);
                io::ip_address::v6_bytes bytes;
                std::memcpy(bytes.data(), sin6->sin6_addr.s6_addr, 16);
                iface->addresses.emplace_back(io::ip_address::v6(bytes, sin6->sin6_scope_id));
            }
        }
    }
    freeifaddrs(ifaddr);
    return interfaces;
}
result<std::string> get_mac_address(std::string_view interface_name) {
    std::string path = "/sys/class/net/" + std::string(interface_name) + "/address";
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::unexpected(
            mirage_error::network(std::format("cannot read MAC for {}", interface_name)));
    }
    std::string mac;
    std::getline(file, mac);
    for (char& c : mac) {
        if (c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    if (!mac.empty() && mac.back() == '\n') {
        mac.pop_back();
    }
    return mac;
}
#elifdef _WIN32
result<std::vector<network_interface>> enumerate_interfaces() {
    std::vector<network_interface> interfaces;
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) {
        return std::unexpected(mirage_error::network("failed to get adapter info size"));
    }
    std::vector<std::byte> buffer(size);
    auto* addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size) != NO_ERROR) {
        return std::unexpected(mirage_error::network("failed to enumerate adapters"));
    }
    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        network_interface iface;
        iface.name = adapter->AdapterName;
        iface.is_up = adapter->OperStatus == IfOperStatusUp;
        iface.is_loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        if (adapter->PhysicalAddressLength == 6) {
            char mac[18];
            std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
                          adapter->PhysicalAddress[0], adapter->PhysicalAddress[1],
                          adapter->PhysicalAddress[2], adapter->PhysicalAddress[3],
                          adapter->PhysicalAddress[4], adapter->PhysicalAddress[5]);
            iface.mac_address = mac;
        }
        for (auto* addr = adapter->FirstUnicastAddress; addr != nullptr; addr = addr->Next) {
            if (addr->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(addr->Address.lpSockaddr);
                iface.addresses.push_back(io::ip_address::v4(ntohl(sin->sin_addr.s_addr)));
            } else if (addr->Address.lpSockaddr->sa_family == AF_INET6) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(addr->Address.lpSockaddr);
                io::ip_address::v6_bytes bytes;
                std::memcpy(bytes.data(), sin6->sin6_addr.s6_addr, 16);
                iface.addresses.push_back(io::ip_address::v6(bytes, sin6->sin6_scope_id));
            }
        }
        interfaces.push_back(std::move(iface));
    }
    return interfaces;
}
result<std::string> get_mac_address(std::string_view interface_name) {
    auto interfaces = enumerate_interfaces();
    if (!interfaces) {
        return std::unexpected(interfaces.error());
    }
    for (const auto& iface : *interfaces) {
        if (iface.name == interface_name) {
            return iface.mac_address;
        }
    }
    return std::unexpected(
        mirage_error::network(std::format("interface not found: {}", interface_name)));
}
#else
#error "Unsupported platform"
#endif
}  // namespace mirage::discovery
