#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/cli_options.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::string_view> args(std::initializer_list<std::string_view> values) {
    return std::vector<std::string_view>(values);
}

std::filesystem::path temp_path(std::string_view name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           std::format("mirage-cli-options-{}-{}.conf", name, stamp);
}

bool write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << content;
    return true;
}

bool message_contains(const mirage::cli_error& error, std::string_view needle) {
    return error.message.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    bool ok = true;

    const auto missing_default = temp_path("missing-default");
    std::error_code ec;
    std::filesystem::remove(missing_default, ec);

    auto default_args =
        args({"--name", "Desk", "--port", "7100", "--cast", "--no-mdns", "--debug", "--daemon"});
    auto defaults = mirage::parse_runtime_cli_options(default_args, missing_default);
    ok &= expect(defaults.has_value(), "default runtime options did not parse");
    if (defaults) {
        ok &= expect(defaults->config_path == missing_default, "default config path mismatch");
        ok &= expect(!defaults->explicit_config, "default config marked explicit");
        ok &= expect(!defaults->config_exists, "missing default config marked present");
        ok &= expect(defaults->cfg.device_name == "Desk", "name override mismatch");
        ok &= expect(defaults->cfg.airplay_port == 7100, "airplay port override mismatch");
        ok &= expect(defaults->cfg.enable_cast, "cast flag mismatch");
        ok &= expect(defaults->no_mdns, "no-mdns flag mismatch");
        ok &= expect(defaults->debug, "debug flag mismatch");
        ok &= expect(defaults->daemon_mode, "daemon flag mismatch");
    }

    auto background_child_args = args({"--background-child"});
    auto background_child =
        mirage::parse_runtime_cli_options(background_child_args, missing_default);
    ok &= expect(background_child.has_value(), "background child option did not parse");
    if (background_child) {
        ok &= expect(background_child->daemon_mode, "background child did not imply daemon");
        ok &= expect(background_child->background_child_mode, "background child flag mismatch");
    }

    const auto config_path = temp_path("configured");
    ok &= expect(write_file(config_path, R"(
name = Living Room
airplay_port = 7001
cast_port = 8001
miracast_port = 7237
enable_airplay = false
enable_cast = true
enable_miracast = false
identity_key = from-config.key
)"),
                 "could not write test config");
    const auto config_path_string = config_path.string();

    auto override_args = args({"--config", config_path_string, "--name", "Override", "--cast-port",
                               "8100", "--no-cast", "--miracast", "--identity-key", "from-cli.key",
                               "--trace", "--diagnostics"});
    auto overrides = mirage::parse_runtime_cli_options(override_args, missing_default);
    ok &= expect(overrides.has_value(), "configured runtime options did not parse");
    if (overrides) {
        ok &= expect(overrides->explicit_config, "explicit config not marked");
        ok &= expect(overrides->config_exists, "explicit config not marked present");
        ok &= expect(overrides->config_path == config_path, "explicit config path mismatch");
        ok &= expect(overrides->cfg.device_name == "Override", "config name override mismatch");
        ok &= expect(overrides->cfg.airplay_port == 7001, "config airplay port mismatch");
        ok &= expect(overrides->cfg.cast_port == 8100, "cast port override mismatch");
        ok &= expect(overrides->cfg.miracast_port == 7237, "config miracast port mismatch");
        ok &= expect(!overrides->cfg.enable_airplay, "config airplay enable mismatch");
        ok &= expect(!overrides->cfg.enable_cast, "no-cast override mismatch");
        ok &= expect(overrides->cfg.enable_miracast, "miracast override mismatch");
        ok &= expect(overrides->cfg.identity_key_path == "from-cli.key",
                     "identity key override mismatch");
        ok &= expect(overrides->trace, "trace flag mismatch");
        ok &= expect(overrides->diagnostics, "diagnostics flag mismatch");
    }

    auto bad_port_args = args({"--port", "not-a-port"});
    auto bad_port = mirage::parse_runtime_cli_options(bad_port_args, missing_default);
    ok &= expect(!bad_port, "bad port unexpectedly parsed");
    if (!bad_port) {
        ok &= expect(bad_port.error().exit_code() == 2, "bad port exit code mismatch");
        ok &= expect(message_contains(bad_port.error(), "--port"), "bad port error missing option");
    }

    auto missing_value_args = args({"--identity-key"});
    auto missing_value = mirage::parse_runtime_cli_options(missing_value_args, missing_default);
    ok &= expect(!missing_value, "missing value unexpectedly parsed");
    if (!missing_value) {
        ok &= expect(missing_value.error().exit_code() == 2, "missing value exit code mismatch");
        ok &= expect(message_contains(missing_value.error(), "--identity-key"),
                     "missing value error missing option");
    }

    const auto absent_config = temp_path("absent");
    std::filesystem::remove(absent_config, ec);
    const auto absent_config_string = absent_config.string();
    auto absent_config_args = args({"--config", absent_config_string});
    auto absent = mirage::parse_runtime_cli_options(absent_config_args, missing_default);
    ok &= expect(!absent, "absent explicit config unexpectedly parsed");
    if (!absent) {
        ok &= expect(absent.error().exit_code() == 1, "absent config exit code mismatch");
        ok &= expect(message_contains(absent.error(), "config file not found"),
                     "absent config error mismatch");
    }

    auto unknown_args = args({"launch"});
    auto unknown = mirage::parse_runtime_cli_options(unknown_args, missing_default);
    ok &= expect(!unknown, "unknown option unexpectedly parsed");
    if (!unknown) {
        ok &= expect(unknown.error().exit_code() == 2, "unknown option exit code mismatch");
        ok &= expect(message_contains(unknown.error(), "launch"),
                     "unknown option error missing value");
    }

    auto paths_args = args({"--config", config_path_string});
    auto paths = mirage::parse_paths_cli_options(paths_args, missing_default);
    ok &= expect(paths.has_value(), "paths options did not parse");
    if (paths) {
        ok &= expect(paths->explicit_config, "paths config not marked explicit");
        ok &= expect(paths->config_path == config_path, "paths config mismatch");
    }

    auto bad_paths_args = args({"--port", "7000"});
    auto bad_paths = mirage::parse_paths_cli_options(bad_paths_args, missing_default);
    ok &= expect(!bad_paths, "bad paths options unexpectedly parsed");
    if (!bad_paths) {
        ok &= expect(bad_paths.error().exit_code() == 2, "bad paths exit code mismatch");
    }

    auto max_port = mirage::parse_cli_port("--port", "65535");
    auto too_large_port = mirage::parse_cli_port("--port", "65536");
    ok &= expect(max_port && *max_port == 65535, "max port parse mismatch");
    ok &= expect(!too_large_port, "too large port unexpectedly parsed");

    std::filesystem::remove(config_path, ec);
    return ok ? 0 : 1;
}
