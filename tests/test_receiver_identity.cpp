#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "core/receiver_identity.hpp"
#include "discovery/discovery.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::string txt_value(const mirage::discovery::service_record& record, std::string_view key) {
    auto it = std::ranges::find(record.txt_records, key, &std::pair<std::string, std::string>::first);
    if (it == record.txt_records.end()) {
        return {};
    }
    return it->second;
}

bool is_upper_hex(std::string_view value) {
    return std::ranges::all_of(value, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
    });
}

}  // namespace

int main() {
    bool ok = true;

    std::array<std::byte, 32> public_key{};
    for (size_t i = 0; i < public_key.size(); ++i) {
        public_key[i] = static_cast<std::byte>(i);
    }

    auto cast_identity_a = mirage::derive_protocol_identity(public_key, "cast-v2");
    auto cast_identity_b = mirage::derive_protocol_identity(public_key, "cast-v2");
    auto airplay_identity = mirage::derive_protocol_identity(public_key, "airplay");

    ok &= expect(cast_identity_a.stable_id == cast_identity_b.stable_id,
                 "cast stable id is not deterministic");
    ok &= expect(cast_identity_a.uuid == cast_identity_b.uuid, "cast uuid is not deterministic");
    ok &= expect(cast_identity_a.short_id == cast_identity_b.short_id,
                 "cast short id is not deterministic");
    ok &= expect(cast_identity_a.bootstrap_id == cast_identity_b.bootstrap_id,
                 "cast bootstrap id is not deterministic");
    ok &= expect(cast_identity_a.uuid != airplay_identity.uuid,
                 "protocol identities should be separated");

    ok &= expect(cast_identity_a.stable_id.size() == 32, "stable id length mismatch");
    ok &= expect(cast_identity_a.uuid.size() == 36, "uuid length mismatch");
    ok &= expect(cast_identity_a.uuid[8] == '-' && cast_identity_a.uuid[13] == '-' &&
                     cast_identity_a.uuid[18] == '-' && cast_identity_a.uuid[23] == '-',
                 "uuid separator mismatch");
    ok &= expect(cast_identity_a.uuid[14] == '8', "uuid version mismatch");
    ok &= expect(cast_identity_a.short_id.size() == 8 && is_upper_hex(cast_identity_a.short_id),
                 "short id format mismatch");
    ok &= expect(cast_identity_a.bootstrap_id.size() == 12 &&
                     is_upper_hex(cast_identity_a.bootstrap_id),
                 "bootstrap id format mismatch");

    auto cast_service_a =
        mirage::discovery::create_cast_service("Living Room", 8009, cast_identity_a);
    auto cast_service_b =
        mirage::discovery::create_cast_service("Living Room", 8009, cast_identity_a);
    ok &= expect(txt_value(cast_service_a, "id") == cast_identity_a.uuid,
                 "cast service id mismatch");
    ok &= expect(txt_value(cast_service_a, "cd") == cast_identity_a.short_id,
                 "cast service cd mismatch");
    ok &= expect(txt_value(cast_service_a, "bs") == cast_identity_a.bootstrap_id,
                 "cast service bs mismatch");
    ok &= expect(txt_value(cast_service_a, "id") == txt_value(cast_service_b, "id"),
                 "cast service id changed between calls");
    ok &= expect(txt_value(cast_service_a, "cd") == txt_value(cast_service_b, "cd"),
                 "cast service cd changed between calls");
    ok &= expect(txt_value(cast_service_a, "bs") == txt_value(cast_service_b, "bs"),
                 "cast service bs changed between calls");

    auto airplay_service_a = mirage::discovery::create_airplay_service(
        "Living Room", 7000, public_key, "AA:BB:CC:DD:EE:FF");
    auto airplay_service_b = mirage::discovery::create_airplay_service(
        "Living Room", 7000, public_key, "AA:BB:CC:DD:EE:FF");
    ok &= expect(txt_value(airplay_service_a, "pi") == airplay_identity.uuid,
                 "airplay pi mismatch");
    ok &= expect(txt_value(airplay_service_a, "pi") == txt_value(airplay_service_b, "pi"),
                 "airplay pi changed between calls");

    public_key[0] = std::byte{0xAA};
    auto airplay_service_c = mirage::discovery::create_airplay_service(
        "Living Room", 7000, public_key, "AA:BB:CC:DD:EE:FF");
    ok &= expect(txt_value(airplay_service_a, "pi") != txt_value(airplay_service_c, "pi"),
                 "airplay pi did not change for a different identity");

    return ok ? 0 : 1;
}
