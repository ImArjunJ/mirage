#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/doctor_checks.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    for (const auto& value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

mirage::doctor::environment_reader map_environment(
    std::map<std::string, std::string> values) {
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        auto it = values.find(std::string(name));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

}  // namespace

int main() {
    bool ok = true;

    std::vector<std::string> names{"alpha", "beta", "gamma"};
    ok &= expect(mirage::doctor::join_names(names) == "alpha/beta/gamma",
                 "join_names should use slash separators");
    std::vector<std::string> empty;
    ok &= expect(mirage::doctor::join_names(empty) == "none",
                 "join_names should name empty lists");

    const auto compiled_windows = mirage::doctor::compiled_window_backends();
    const auto compiled_audio = mirage::doctor::compiled_audio_backends();
    ok &= expect(!compiled_windows.empty(), "compiled window backends should not be empty");
    ok &= expect(!compiled_audio.empty(), "compiled audio backends should not be empty");

#ifdef _WIN32
    auto no_env = map_environment({});
    ok &= expect(contains(mirage::doctor::detected_window_backends(no_env), "win32"),
                 "windows should report win32 window backend");
    ok &= expect(contains(mirage::doctor::detected_audio_hints(no_env), "wasapi"),
                 "windows should report wasapi audio backend");
#else
    auto desktop_env = map_environment({
        {"WAYLAND_DISPLAY", "wayland-0"},
        {"DISPLAY", ":0"},
        {"PULSE_SERVER", "unix:/tmp/pulse"},
    });
    const auto windows = mirage::doctor::detected_window_backends(desktop_env);
    ok &= expect(contains(windows, "wayland"), "wayland display should be detected");
    ok &= expect(contains(windows, "x11"), "x11 display should be detected");
    ok &= expect(contains(mirage::doctor::detected_audio_hints(desktop_env),
                          "pulseaudio/pipewire server"),
                 "pulse server should be detected");
#endif

    auto hints = mirage::doctor::collect_backend_hints(map_environment({
        {"WAYLAND_DISPLAY", "wayland-0"},
        {"PULSE_SERVER", "unix:/tmp/pulse"},
    }));
    ok &= expect(hints.size() == 2, "backend hint count mismatch");
    for (const auto& hint : hints) {
        ok &= expect(!hint.name.empty(), "backend hint name missing");
        ok &= expect(!hint.detail.empty(), "backend hint detail missing");
    }

    return ok ? 0 : 1;
}
