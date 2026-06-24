#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mirage::doctor {

enum class check_level { ok, note, error };

struct check_result {
    std::string name;
    check_level level = check_level::ok;
    std::string detail;
    std::string fix;
};

using environment_reader = std::function<std::optional<std::string>(std::string_view)>;

[[nodiscard]] std::vector<std::string> compiled_window_backends();
[[nodiscard]] std::vector<std::string> compiled_audio_backends();
[[nodiscard]] std::vector<std::string> detected_window_backends(const environment_reader& env);
[[nodiscard]] std::vector<std::string> detected_audio_hints(const environment_reader& env);
[[nodiscard]] std::string join_names(std::span<const std::string> names);

[[nodiscard]] std::vector<check_result> collect_runtime_checks();
[[nodiscard]] std::vector<check_result> collect_asset_checks();
[[nodiscard]] std::vector<check_result> collect_backend_hints(const environment_reader& env);

}  // namespace mirage::doctor
