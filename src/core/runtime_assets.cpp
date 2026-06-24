#include "core/runtime_assets.hpp"

#include <array>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef MIRAGE_BUILD_SHADER_DIR
#define MIRAGE_BUILD_SHADER_DIR ""
#endif

#ifndef MIRAGE_INSTALL_SHADER_DIR
#define MIRAGE_INSTALL_SHADER_DIR ""
#endif

namespace mirage::assets {

std::optional<std::filesystem::path> executable_dir() {
#ifdef _WIN32
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = 0;
    for (;;) {
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(size) < buffer.size()) {
            buffer.resize(static_cast<size_t>(size));
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
#else
    std::array<char, 4096> buffer{};
    const auto size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (size <= 0) {
        return std::nullopt;
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<size_t>(size)))
        .parent_path();
#endif
}

std::vector<std::filesystem::path> shader_search_dirs() {
    std::vector<std::filesystem::path> search_dirs;
    if (const char* env = std::getenv("MIRAGE_SHADER_DIR");
        env != nullptr && env[0] != '\0') {
        search_dirs.emplace_back(env);
    }
    if (auto dir = executable_dir()) {
        search_dirs.emplace_back(*dir / "shaders");
        if constexpr (std::string_view(MIRAGE_INSTALL_SHADER_DIR).size() > 0) {
            search_dirs.emplace_back(*dir / ".." / MIRAGE_INSTALL_SHADER_DIR);
        }
    }
    if constexpr (std::string_view(MIRAGE_BUILD_SHADER_DIR).size() > 0) {
        search_dirs.emplace_back(MIRAGE_BUILD_SHADER_DIR);
    }
    search_dirs.emplace_back(std::filesystem::current_path() / "shaders");
    return search_dirs;
}

std::optional<std::filesystem::path> locate_shader(std::string_view filename) {
    for (const auto& dir : shader_search_dirs()) {
        auto candidate = dir / filename;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> required_shader_names() {
    return {"nv12_to_rgb.vert.spv", "nv12_to_rgb.frag.spv"};
}

}  // namespace mirage::assets
