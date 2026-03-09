#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include "core/core.hpp"
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
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') {
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') {
            key.pop_back();
        }
        while (!val.empty() && val.front() == ' ') {
            val.erase(val.begin());
        }
        while (!val.empty() && (val.front() == '"' || val.front() == '\'')) {
            val.erase(val.begin());
        }
        while (!val.empty() && (val.back() == '"' || val.back() == '\'')) {
            val.pop_back();
        }
        if (key == "name") {
            cfg.device_name = val;
        } else if (key == "airplay_port") {
            cfg.airplay_port = static_cast<uint16_t>(std::stoi(val));
        } else if (key == "cast_port") {
            cfg.cast_port = static_cast<uint16_t>(std::stoi(val));
        } else if (key == "miracast_port") {
            cfg.miracast_port = static_cast<uint16_t>(std::stoi(val));
        } else if (key == "enable_airplay") {
            cfg.enable_airplay = (val == "true" || val == "1");
        } else if (key == "enable_cast") {
            cfg.enable_cast = (val == "true" || val == "1");
        } else if (key == "enable_miracast") {
            cfg.enable_miracast = (val == "true" || val == "1");
        } else if (key == "hardware_decode") {
            cfg.hardware_decode = (val == "true" || val == "1");
        } else if (key == "log_level") {
            cfg.log_level = val;
        }
    }
    return cfg;
}
}  // namespace mirage
