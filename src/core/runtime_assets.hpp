#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace mirage::assets {

[[nodiscard]] std::optional<std::filesystem::path> executable_dir();
[[nodiscard]] std::vector<std::filesystem::path> shader_search_dirs();
[[nodiscard]] std::optional<std::filesystem::path> locate_shader(std::string_view filename);
[[nodiscard]] std::vector<std::string_view> required_shader_names();

}  // namespace mirage::assets
