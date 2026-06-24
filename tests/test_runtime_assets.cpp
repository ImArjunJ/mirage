#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "core/runtime_assets.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains_name(const std::vector<std::string_view>& values, std::string_view needle) {
    for (auto value : values) {
        if (value == needle) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    bool ok = true;

    const auto names = mirage::assets::required_shader_names();
    ok &= expect(contains_name(names, "nv12_to_rgb.vert.spv"),
                 "required vertex shader missing");
    ok &= expect(contains_name(names, "nv12_to_rgb.frag.spv"),
                 "required fragment shader missing");

    const auto dirs = mirage::assets::shader_search_dirs();
    ok &= expect(!dirs.empty(), "shader search dirs should not be empty");

    for (auto name : names) {
        const auto path = mirage::assets::locate_shader(name);
        ok &= expect(path.has_value(), "required shader should be discoverable");
        if (path) {
            std::error_code ec;
            ok &= expect(std::filesystem::is_regular_file(*path, ec),
                         "shader path should be a regular file");
        }
    }

    return ok ? 0 : 1;
}
