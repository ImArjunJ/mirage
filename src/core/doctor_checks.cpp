#include "core/doctor_checks.hpp"

#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <vulkan/vulkan.h>

#include <openssl/crypto.h>

#include "core/runtime_assets.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

namespace mirage::doctor {
namespace {

bool has_value(const std::optional<std::string>& value) {
    return value && !value->empty();
}

std::string version_string(unsigned version) {
    return std::format("{}.{}.{}", AV_VERSION_MAJOR(version), AV_VERSION_MINOR(version),
                       AV_VERSION_MICRO(version));
}

struct required_decoder {
    AVCodecID id;
    std::string_view name;
};

check_result ffmpeg_check() {
    std::vector<std::string> missing;
    constexpr required_decoder decoders[] = {
        {.id = AV_CODEC_ID_AAC, .name = "aac"},
        {.id = AV_CODEC_ID_ALAC, .name = "alac"},
        {.id = AV_CODEC_ID_H264, .name = "h264"},
    };

    for (const auto& decoder : decoders) {
        if (avcodec_find_decoder(decoder.id) == nullptr) {
            missing.emplace_back(decoder.name);
        }
    }

    if (!missing.empty()) {
        return {
            .name = "ffmpeg",
            .level = check_level::error,
            .detail = std::format("missing decoders: {}", join_names(missing)),
            .fix = "install ffmpeg/libavcodec with aac, alac, and h264 decoder support",
        };
    }

    return {
        .name = "ffmpeg",
        .level = check_level::ok,
        .detail = std::format("libavcodec {}, libavformat {}, libswresample {}; "
                              "aac/alac/h264 decoders available",
                              version_string(avcodec_version()), version_string(avformat_version()),
                              version_string(swresample_version())),
        .fix = {},
    };
}

check_result openssl_check() {
    return {
        .name = "openssl",
        .level = check_level::ok,
        .detail = OpenSSL_version(OPENSSL_VERSION),
        .fix = {},
    };
}

check_result vulkan_check() {
    uint32_t version = 0;
    const auto result = vkEnumerateInstanceVersion(&version);
    if (result != VK_SUCCESS) {
        return {
            .name = "vulkan",
            .level = check_level::error,
            .detail = std::format("loader unavailable: VkResult {}", static_cast<int>(result)),
            .fix = "install a Vulkan loader and GPU driver",
        };
    }

    return {
        .name = "vulkan",
        .level = check_level::ok,
        .detail = std::format("loader {}.{}.{}", VK_VERSION_MAJOR(version),
                              VK_VERSION_MINOR(version), VK_VERSION_PATCH(version)),
        .fix = {},
    };
}

std::vector<std::string> asset_names() {
    std::vector<std::string> names;
    for (auto shader : assets::required_shader_names()) {
        names.emplace_back(shader);
    }
    return names;
}

check_result shader_assets_check() {
    std::vector<std::string> missing;
    std::optional<std::filesystem::path> common_dir;
    bool same_dir = true;

    for (auto shader : assets::required_shader_names()) {
        auto path = assets::locate_shader(shader);
        if (!path) {
            missing.emplace_back(shader);
            continue;
        }
        auto parent = path->parent_path();
        if (!common_dir) {
            common_dir = parent;
        } else if (*common_dir != parent) {
            same_dir = false;
        }
    }

    if (!missing.empty()) {
        return {
            .name = "shaders",
            .level = check_level::error,
            .detail = std::format("missing {}", join_names(missing)),
            .fix = "set MIRAGE_SHADER_DIR or install the mirage shader files",
        };
    }

    auto names = asset_names();
    return {
        .name = "shaders",
        .level = check_level::ok,
        .detail =
            same_dir && common_dir
                ? std::format("{} found in {}", join_names(names),
                              common_dir->lexically_normal().string())
                : std::format("{} found", join_names(names)),
        .fix = {},
    };
}

}  // namespace

std::vector<std::string> compiled_window_backends() {
#ifdef _WIN32
    return {"win32"};
#else
    return {"wayland", "x11"};
#endif
}

std::vector<std::string> compiled_audio_backends() {
#ifdef _WIN32
    return {"wasapi"};
#else
    return {"pulseaudio", "alsa"};
#endif
}

std::vector<std::string> detected_window_backends(const environment_reader& env) {
#ifdef _WIN32
    (void)env;
    return {"win32"};
#else
    std::vector<std::string> backends;
    if (has_value(env("WAYLAND_DISPLAY"))) {
        backends.emplace_back("wayland");
    }
    if (has_value(env("DISPLAY"))) {
        backends.emplace_back("x11");
    }
    return backends;
#endif
}

std::vector<std::string> detected_audio_hints(const environment_reader& env) {
#ifdef _WIN32
    (void)env;
    return {"wasapi"};
#else
    std::vector<std::string> hints;
    if (has_value(env("PULSE_SERVER"))) {
        hints.emplace_back("pulseaudio/pipewire server");
    }
    if (auto runtime_dir = env("XDG_RUNTIME_DIR"); has_value(runtime_dir)) {
        std::error_code ec;
        auto pulse_socket = std::filesystem::path(*runtime_dir) / "pulse" / "native";
        if (std::filesystem::exists(pulse_socket, ec)) {
            hints.emplace_back("pulseaudio/pipewire socket");
        }
    }
    std::error_code ec;
    if (std::filesystem::exists("/dev/snd", ec) ||
        std::filesystem::exists("/proc/asound/cards", ec)) {
        hints.emplace_back("alsa device");
    }
    return hints;
#endif
}

std::string join_names(std::span<const std::string> names) {
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += "/";
        }
        joined += name;
    }
    return joined.empty() ? "none" : joined;
}

std::vector<check_result> collect_runtime_checks() {
    return {openssl_check(), ffmpeg_check(), vulkan_check()};
}

std::vector<check_result> collect_asset_checks() {
    return {shader_assets_check()};
}

std::vector<check_result> collect_backend_hints(const environment_reader& env) {
    std::vector<check_result> checks;

    const auto compiled_windows = compiled_window_backends();
    const auto active_windows = detected_window_backends(env);
    checks.push_back({
        .name = "window",
        .level = active_windows.empty() ? check_level::note : check_level::ok,
        .detail =
            active_windows.empty()
                ? std::format("compiled {}; no display environment detected",
                              join_names(compiled_windows))
                : std::format("{} available (compiled {})", join_names(active_windows),
                              join_names(compiled_windows)),
        .fix = active_windows.empty()
                   ? "set WAYLAND_DISPLAY or DISPLAY before starting video mirroring"
                   : "",
    });

    const auto compiled_audio = compiled_audio_backends();
    const auto audio_hints = detected_audio_hints(env);
    checks.push_back({
        .name = "audio",
        .level = audio_hints.empty() ? check_level::note : check_level::ok,
        .detail = audio_hints.empty()
                      ? std::format("compiled {}; no local audio device hint detected",
                                    join_names(compiled_audio))
                      : std::format("{} detected (compiled {})", join_names(audio_hints),
                                    join_names(compiled_audio)),
        .fix = audio_hints.empty() ? "start pulseaudio/pipewire or expose an alsa device" : "",
    });

    return checks;
}

}  // namespace mirage::doctor
