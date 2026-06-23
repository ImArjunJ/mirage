#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>

#include "core/core.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::filesystem::path temp_config_path(std::string_view name) {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           std::format("mirage-config-{}-{}.conf", name, stamp);
}

bool write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << content;
    return true;
}

bool message_contains(const mirage::mirage_error& error, std::string_view needle) {
    return error.message.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    bool ok = true;

    const auto valid_path = temp_config_path("valid");
    ok &= expect(write_file(valid_path, R"(
# comment
[receiver]
name = "Office Display #1" # keep hash inside the quoted value
airplay_port = 7100
cast_port = 8100 # inline comment
miracast_port = '7300'
enable_airplay = off
enable_cast = yes
enable_miracast = ON
hardware_decode = disabled
log_level = debug
identity_key = '/tmp/mirage identity.key'
unknown_key = ignored
)"),
                 "could not write valid config");

    auto valid = mirage::config::load_from_file(valid_path.string());
    ok &= expect(valid.has_value(), "valid config did not parse");
    if (valid) {
        ok &= expect(valid->device_name == "Office Display #1", "name parse mismatch");
        ok &= expect(valid->airplay_port == 7100, "airplay port parse mismatch");
        ok &= expect(valid->cast_port == 8100, "cast port parse mismatch");
        ok &= expect(valid->miracast_port == 7300, "miracast port parse mismatch");
        ok &= expect(!valid->enable_airplay, "enable_airplay parse mismatch");
        ok &= expect(valid->enable_cast, "enable_cast parse mismatch");
        ok &= expect(valid->enable_miracast, "enable_miracast parse mismatch");
        ok &= expect(!valid->hardware_decode, "hardware_decode parse mismatch");
        ok &= expect(valid->log_level == "debug", "log level parse mismatch");
        ok &= expect(valid->identity_key_path == "/tmp/mirage identity.key",
                     "identity key parse mismatch");
    }

    const auto bad_port_path = temp_config_path("bad-port");
    ok &= expect(write_file(bad_port_path, "airplay_port = nope\n"),
                 "could not write bad port config");
    auto bad_port = mirage::config::load_from_file(bad_port_path.string());
    ok &= expect(!bad_port, "invalid port unexpectedly parsed");
    if (!bad_port) {
        ok &= expect(message_contains(bad_port.error(), "airplay_port"),
                     "invalid port error did not include key");
    }

    const auto range_path = temp_config_path("range");
    ok &= expect(write_file(range_path, "cast_port = 70000\n"),
                 "could not write range config");
    auto range = mirage::config::load_from_file(range_path.string());
    ok &= expect(!range, "out-of-range port unexpectedly parsed");
    if (!range) {
        ok &= expect(message_contains(range.error(), "70000"),
                     "out-of-range error did not include value");
    }

    const auto bad_bool_path = temp_config_path("bad-bool");
    ok &= expect(write_file(bad_bool_path, "enable_cast = sometimes\n"),
                 "could not write bad bool config");
    auto bad_bool = mirage::config::load_from_file(bad_bool_path.string());
    ok &= expect(!bad_bool, "invalid boolean unexpectedly parsed");
    if (!bad_bool) {
        ok &= expect(message_contains(bad_bool.error(), "enable_cast"),
                     "invalid boolean error did not include key");
    }

    std::error_code ec;
    std::filesystem::remove(valid_path, ec);
    std::filesystem::remove(bad_port_path, ec);
    std::filesystem::remove(range_path, ec);
    std::filesystem::remove(bad_bool_path, ec);
    return ok ? 0 : 1;
}
