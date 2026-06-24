#include "core/cli_options.hpp"

#include <charconv>
#include <cstdint>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

bool runtime_flag_option(std::string_view option) {
    return option == "--no-airplay" || option == "--cast" || option == "--miracast" ||
           option == "--no-cast" || option == "--no-miracast" || option == "--no-mdns" ||
           option == "--daemon" || option == "-d" || option == "--diagnostics" ||
           option == "--debug" || option == "--trace" || option == "--verbose" || option == "-v" ||
           option == "--background-child";
}

mirage::cli_error usage_error(std::string message) {
    return {.kind = mirage::cli_error_kind::usage, .message = std::move(message)};
}

mirage::cli_error config_error(std::string message) {
    return {.kind = mirage::cli_error_kind::config, .message = std::move(message)};
}

}  // namespace

namespace mirage {

cli_result<uint16_t> parse_cli_port(std::string_view option, std::string_view value) {
    uint32_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed > 65535) {
        return std::unexpected(
            usage_error(std::format("{} expects a port from 0 to 65535, got '{}'", option, value)));
    }
    return static_cast<uint16_t>(parsed);
}

bool runtime_option_takes_value(std::string_view option) {
    return option == "--config" || option == "--name" || option == "--port" ||
           option == "--cast-port" || option == "--miracast-port" || option == "--identity-key";
}

cli_result<runtime_cli_options> parse_runtime_cli_options(
    std::span<const std::string_view> args, const std::filesystem::path& default_config_path) {
    runtime_cli_options options;
    options.config_path = default_config_path;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (runtime_option_takes_value(arg)) {
            if (i + 1 >= args.size()) {
                return std::unexpected(usage_error(std::format("missing value for {}", arg)));
            }
            if (arg == "--config") {
                options.config_path = args[++i];
                options.explicit_config = true;
            } else {
                ++i;
            }
        } else if (!runtime_flag_option(arg)) {
            return std::unexpected(usage_error(std::format("unknown option: {}", arg)));
        }
    }

    options.config_exists = std::filesystem::exists(options.config_path);
    if (options.config_exists) {
        auto loaded = config::load_from_file(options.config_path.string());
        if (!loaded) {
            return std::unexpected(config_error(loaded.error().message));
        }
        options.cfg = *loaded;
    } else if (options.explicit_config) {
        return std::unexpected(
            config_error(std::format("config file not found: {}", options.config_path.string())));
    }

    for (size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--config") {
            ++i;
        } else if (arg == "--name") {
            options.cfg.device_name = args[++i];
        } else if (arg == "--port") {
            auto parsed = parse_cli_port(arg, args[++i]);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            options.cfg.airplay_port = *parsed;
        } else if (arg == "--cast-port") {
            auto parsed = parse_cli_port(arg, args[++i]);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            options.cfg.cast_port = *parsed;
        } else if (arg == "--miracast-port") {
            auto parsed = parse_cli_port(arg, args[++i]);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            options.cfg.miracast_port = *parsed;
        } else if (arg == "--identity-key") {
            options.cfg.identity_key_path = args[++i];
        } else if (arg == "--no-airplay") {
            options.cfg.enable_airplay = false;
        } else if (arg == "--cast") {
            options.cfg.enable_cast = true;
        } else if (arg == "--miracast") {
            options.cfg.enable_miracast = true;
        } else if (arg == "--no-cast") {
            options.cfg.enable_cast = false;
        } else if (arg == "--no-miracast") {
            options.cfg.enable_miracast = false;
        } else if (arg == "--verbose" || arg == "-v") {
            options.verbose = true;
        } else if (arg == "--debug") {
            options.debug = true;
        } else if (arg == "--trace") {
            options.trace = true;
        } else if (arg == "--diagnostics") {
            options.diagnostics = true;
        } else if (arg == "--no-mdns") {
            options.no_mdns = true;
        } else if (arg == "--daemon" || arg == "-d") {
            options.daemon_mode = true;
        } else if (arg == "--background-child") {
            options.background_child_mode = true;
            options.daemon_mode = true;
        }
    }

    return options;
}

cli_result<paths_cli_options> parse_paths_cli_options(
    std::span<const std::string_view> args, const std::filesystem::path& default_config_path) {
    paths_cli_options options{.config_path = default_config_path};
    for (size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        if (arg == "--config") {
            if (i + 1 >= args.size()) {
                return std::unexpected(usage_error("missing value for --config"));
            }
            options.config_path = args[++i];
            options.explicit_config = true;
        } else {
            return std::unexpected(usage_error(std::format("unknown paths option: {}", arg)));
        }
    }
    return options;
}

}  // namespace mirage
