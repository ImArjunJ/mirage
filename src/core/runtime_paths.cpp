#include "core/runtime_paths.hpp"

#include <cstdlib>

namespace mirage {
namespace {

std::optional<std::string> env_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::filesystem::path path_or_empty(const std::optional<std::string>& value) {
    if (!value || value->empty()) {
        return {};
    }
    return std::filesystem::path(*value);
}

}  // namespace

runtime_platform current_runtime_platform() {
#ifdef _WIN32
    return runtime_platform::windows;
#else
    return runtime_platform::posix;
#endif
}

runtime_path_environment current_runtime_path_environment() {
    return {
        .home = env_value("HOME"),
        .xdg_state_home = env_value("XDG_STATE_HOME"),
        .xdg_config_home = env_value("XDG_CONFIG_HOME"),
        .local_app_data = env_value("LOCALAPPDATA"),
        .roaming_app_data = env_value("APPDATA"),
        .user_profile = env_value("USERPROFILE"),
    };
}

std::filesystem::path runtime_state_dir(const runtime_path_environment& env,
                                        runtime_platform platform) {
    if (platform == runtime_platform::windows) {
        if (auto local = path_or_empty(env.local_app_data); !local.empty()) {
            return local / "mirage";
        }
        if (auto profile = path_or_empty(env.user_profile); !profile.empty()) {
            return profile / ".mirage";
        }
        return std::filesystem::path("C:\\ProgramData") / "mirage";
    }

    if (auto state = path_or_empty(env.xdg_state_home); !state.empty()) {
        return state / "mirage";
    }
    auto home = path_or_empty(env.home);
    if (home.empty()) {
        home = "/tmp";
    }
    return home / ".local" / "state" / "mirage";
}

std::filesystem::path runtime_config_dir(const runtime_path_environment& env,
                                         runtime_platform platform) {
    if (platform == runtime_platform::windows) {
        if (auto roaming = path_or_empty(env.roaming_app_data); !roaming.empty()) {
            return roaming / "mirage";
        }
        return runtime_state_dir(env, platform);
    }

    if (auto config = path_or_empty(env.xdg_config_home); !config.empty()) {
        return config / "mirage";
    }
    auto home = path_or_empty(env.home);
    if (home.empty()) {
        home = "/tmp";
    }
    return home / ".config" / "mirage";
}

std::filesystem::path runtime_default_config_file_path(const runtime_path_environment& env,
                                                       runtime_platform platform) {
    return runtime_config_dir(env, platform) / "config.conf";
}

std::filesystem::path runtime_pid_file_path(const runtime_path_environment& env,
                                            runtime_platform platform) {
    return runtime_state_dir(env, platform) / "mirage.pid";
}

std::filesystem::path runtime_status_file_path(const runtime_path_environment& env,
                                               runtime_platform platform) {
    return runtime_state_dir(env, platform) / "status.json";
}

std::filesystem::path runtime_identity_key_path(const config& cfg,
                                                const runtime_path_environment& env,
                                                runtime_platform platform) {
    if (!cfg.identity_key_path.empty()) {
        return std::filesystem::path(cfg.identity_key_path);
    }
    return runtime_state_dir(env, platform) / "identity.key";
}

}  // namespace mirage
