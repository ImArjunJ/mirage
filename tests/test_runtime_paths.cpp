#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "core/runtime_paths.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool path_ends_with(const std::filesystem::path& path, std::string_view suffix) {
    auto normalized = path.generic_string();
    std::ranges::replace(normalized, '\\', '/');
    return normalized.ends_with(suffix);
}

}  // namespace

int main() {
    bool ok = true;

    mirage::runtime_path_environment posix_env;
    posix_env.home = "/home/junie";
    posix_env.xdg_state_home = "/run/state";
    posix_env.xdg_config_home = "/run/config";
    ok &= expect(mirage::runtime_state_dir(posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/state") / "mirage",
                 "posix xdg state path mismatch");
    ok &= expect(mirage::runtime_config_dir(posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/config") / "mirage",
                 "posix xdg config path mismatch");
    ok &= expect(mirage::runtime_default_config_file_path(
                     posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/config") / "mirage" / "config.conf",
                 "posix config file path mismatch");
    ok &= expect(mirage::runtime_pid_file_path(posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/state") / "mirage" / "mirage.pid",
                 "posix pid path mismatch");
    ok &= expect(mirage::runtime_status_file_path(posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/state") / "mirage" / "status.json",
                 "posix status path mismatch");

    mirage::runtime_path_environment posix_home_only;
    posix_home_only.home = "/home/mirage";
    ok &= expect(mirage::runtime_state_dir(posix_home_only, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/home/mirage") / ".local" / "state" / "mirage",
                 "posix home state fallback mismatch");
    ok &= expect(mirage::runtime_config_dir(posix_home_only, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/home/mirage") / ".config" / "mirage",
                 "posix home config fallback mismatch");

    mirage::runtime_path_environment posix_empty;
    ok &= expect(mirage::runtime_state_dir(posix_empty, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/tmp") / ".local" / "state" / "mirage",
                 "posix empty state fallback mismatch");
    ok &= expect(mirage::runtime_config_dir(posix_empty, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/tmp") / ".config" / "mirage",
                 "posix empty config fallback mismatch");

    mirage::runtime_path_environment windows_env;
    windows_env.local_app_data = "C:\\Users\\junie\\AppData\\Local";
    windows_env.roaming_app_data = "C:\\Users\\junie\\AppData\\Roaming";
    windows_env.user_profile = "C:\\Users\\junie";
    ok &= expect(path_ends_with(mirage::runtime_state_dir(
                     windows_env, mirage::runtime_platform::windows),
                                "AppData/Local/mirage"),
                 "windows local app data state path mismatch");
    ok &= expect(path_ends_with(mirage::runtime_config_dir(
                     windows_env, mirage::runtime_platform::windows),
                                "AppData/Roaming/mirage"),
                 "windows app data config path mismatch");
    ok &= expect(path_ends_with(mirage::runtime_default_config_file_path(
                     windows_env, mirage::runtime_platform::windows),
                                "AppData/Roaming/mirage/config.conf"),
                 "windows config file path mismatch");

    mirage::runtime_path_environment windows_profile_only;
    windows_profile_only.user_profile = "C:\\Users\\fallback";
    ok &= expect(path_ends_with(mirage::runtime_state_dir(
                     windows_profile_only, mirage::runtime_platform::windows),
                                "C:/Users/fallback/.mirage"),
                 "windows user profile state fallback mismatch");
    ok &= expect(path_ends_with(mirage::runtime_config_dir(
                     windows_profile_only, mirage::runtime_platform::windows),
                                "C:/Users/fallback/.mirage"),
                 "windows config fallback mismatch");

    mirage::config cfg;
    ok &= expect(mirage::runtime_identity_key_path(
                     cfg, posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/run/state") / "mirage" / "identity.key",
                 "default identity path mismatch");
    cfg.identity_key_path = "/tmp/custom.key";
    ok &= expect(mirage::runtime_identity_key_path(
                     cfg, posix_env, mirage::runtime_platform::posix) ==
                     std::filesystem::path("/tmp/custom.key"),
                 "explicit identity path mismatch");

    return ok ? 0 : 1;
}
