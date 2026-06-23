#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mirage {

enum class runtime_pid_state { missing, invalid, stale, running };

struct runtime_pid_status {
    runtime_pid_state state = runtime_pid_state::missing;
    std::optional<int> pid;
};

struct runtime_claim_result {
    bool claimed = false;
    runtime_pid_status existing;
    std::string error;
};

using runtime_process_probe = std::function<bool(int)>;

[[nodiscard]] std::optional<int> parse_runtime_pid(std::string_view text);
[[nodiscard]] runtime_pid_status read_runtime_pid_status(
    const std::filesystem::path& pid_path, const runtime_process_probe& probe);
[[nodiscard]] bool write_runtime_pid_file(const std::filesystem::path& pid_path, int pid,
                                          std::string* error = nullptr);
void remove_runtime_files(const std::filesystem::path& pid_path,
                          const std::filesystem::path& status_path);
[[nodiscard]] runtime_claim_result claim_runtime_files(
    const std::filesystem::path& pid_path, const std::filesystem::path& status_path,
    int current_pid, const runtime_process_probe& probe);

[[nodiscard]] int current_process_id();
[[nodiscard]] bool is_process_running(int pid);
[[nodiscard]] bool request_process_stop(int pid);
[[nodiscard]] bool wait_for_process_exit(
    int pid, std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(100));

}  // namespace mirage
