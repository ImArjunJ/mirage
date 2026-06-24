#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "core/core.hpp"

namespace mirage {

enum class cli_error_kind { usage, config };

struct cli_error {
    cli_error_kind kind = cli_error_kind::usage;
    std::string message;

    [[nodiscard]] int exit_code() const { return kind == cli_error_kind::usage ? 2 : 1; }
};

template <typename T>
using cli_result = std::expected<T, cli_error>;

struct runtime_cli_options {
    config cfg;
    std::filesystem::path config_path;
    bool explicit_config = false;
    bool config_exists = false;
    bool verbose = false;
    bool debug = false;
    bool trace = false;
    bool diagnostics = false;
    bool no_mdns = false;
    bool daemon_mode = false;
    bool background_child_mode = false;
};

struct paths_cli_options {
    std::filesystem::path config_path;
    bool explicit_config = false;
};

[[nodiscard]] cli_result<uint16_t> parse_cli_port(std::string_view option, std::string_view value);
[[nodiscard]] bool runtime_option_takes_value(std::string_view option);
[[nodiscard]] cli_result<runtime_cli_options> parse_runtime_cli_options(
    std::span<const std::string_view> args, const std::filesystem::path& default_config_path);
[[nodiscard]] cli_result<paths_cli_options> parse_paths_cli_options(
    std::span<const std::string_view> args, const std::filesystem::path& default_config_path);

}  // namespace mirage
