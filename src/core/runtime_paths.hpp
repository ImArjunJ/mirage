#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "core/core.hpp"

namespace mirage {

enum class runtime_platform : uint8_t { posix, windows };

struct runtime_path_environment {
    std::optional<std::string> home;
    std::optional<std::string> xdg_state_home;
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> local_app_data;
    std::optional<std::string> roaming_app_data;
    std::optional<std::string> user_profile;
};

[[nodiscard]] runtime_platform current_runtime_platform();
[[nodiscard]] runtime_path_environment current_runtime_path_environment();

[[nodiscard]] std::filesystem::path runtime_state_dir(
    const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());
[[nodiscard]] std::filesystem::path runtime_config_dir(
    const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());
[[nodiscard]] std::filesystem::path runtime_default_config_file_path(
    const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());
[[nodiscard]] std::filesystem::path runtime_pid_file_path(
    const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());
[[nodiscard]] std::filesystem::path runtime_status_file_path(
    const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());
[[nodiscard]] std::filesystem::path runtime_identity_key_path(
    const config& cfg, const runtime_path_environment& env,
    runtime_platform platform = current_runtime_platform());

}  // namespace mirage
