#include <cstdint>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include "core/core.hpp"

namespace {

bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim_ascii(std::string_view value) {
    while (!value.empty() && ascii_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && ascii_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::string strip_inline_comment(std::string_view value) {
    char quote = '\0';
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if ((c == '"' || c == '\'') && (quote == '\0' || quote == c)) {
            quote = quote == '\0' ? c : '\0';
            out.push_back(c);
            continue;
        }
        if (c == '#' && quote == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

std::string clean_value(std::string_view value) {
    auto without_comment = strip_inline_comment(value);
    auto trimmed = trim_ascii(without_comment);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '"' && trimmed.back() == '"') ||
         (trimmed.front() == '\'' && trimmed.back() == '\''))) {
        trimmed.remove_prefix(1);
        trimmed.remove_suffix(1);
    }
    return std::string(trim_ascii(trimmed));
}

std::string lower_ascii(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

mirage::result<uint16_t> parse_port(std::string_view key, std::string_view value,
                                    size_t line_number) {
    uint32_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc{} || ptr != end || parsed > 65535) {
        return std::unexpected(mirage::mirage_error::config_err(std::format(
            "invalid port for {} on line {}: {}", key, line_number, value)));
    }
    return static_cast<uint16_t>(parsed);
}

mirage::result<bool> parse_bool(std::string_view key, std::string_view value,
                                size_t line_number) {
    auto normalized = lower_ascii(trim_ascii(value));
    if (normalized == "true" || normalized == "1" || normalized == "yes" ||
        normalized == "on" || normalized == "enabled") {
        return true;
    }
    if (normalized == "false" || normalized == "0" || normalized == "no" ||
        normalized == "off" || normalized == "disabled") {
        return false;
    }
    return std::unexpected(mirage::mirage_error::config_err(
        std::format("invalid boolean for {} on line {}: {}", key, line_number, value)));
}

}  // namespace

namespace mirage {
result<config> config::load_from_file(std::string_view path) {
    if (!std::filesystem::exists(path)) {
        return std::unexpected(
            mirage_error::config_err(std::format("config file not found: {}", path)));
    }
    auto file = std::ifstream{std::string(path)};
    if (!file) {
        return std::unexpected(
            mirage_error::config_err(std::format("cannot open config file: {}", path)));
    }
    config cfg;
    std::string line;
    size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        auto trimmed_line = trim_ascii(line);
        if (trimmed_line.empty() || trimmed_line.front() == '#' || trimmed_line.front() == '[') {
            continue;
        }
        auto eq = trimmed_line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        auto key_view = trim_ascii(trimmed_line.substr(0, eq));
        auto val = clean_value(trimmed_line.substr(eq + 1));
        auto key = std::string(key_view);
        if (key == "name") {
            cfg.device_name = val;
        } else if (key == "airplay_port") {
            auto parsed = parse_port(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.airplay_port = *parsed;
        } else if (key == "cast_port") {
            auto parsed = parse_port(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.cast_port = *parsed;
        } else if (key == "miracast_port") {
            auto parsed = parse_port(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.miracast_port = *parsed;
        } else if (key == "enable_airplay") {
            auto parsed = parse_bool(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.enable_airplay = *parsed;
        } else if (key == "enable_cast") {
            auto parsed = parse_bool(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.enable_cast = *parsed;
        } else if (key == "enable_miracast") {
            auto parsed = parse_bool(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.enable_miracast = *parsed;
        } else if (key == "hardware_decode") {
            auto parsed = parse_bool(key, val, line_number);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            cfg.hardware_decode = *parsed;
        } else if (key == "log_level") {
            cfg.log_level = val;
        } else if (key == "identity_key" || key == "identity_key_path") {
            cfg.identity_key_path = val;
        }
    }
    return cfg;
}
}  // namespace mirage
