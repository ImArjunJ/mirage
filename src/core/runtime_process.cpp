#include "core/runtime_process.hpp"

#include <charconv>
#include <fstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace mirage {
namespace {

bool is_ascii_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::filesystem::path tmp_status_path(const std::filesystem::path& status_path) {
    auto tmp = status_path;
    tmp += ".tmp";
    return tmp;
}

}  // namespace

std::optional<int> parse_runtime_pid(std::string_view text) {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }

    int pid = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, pid);
    if (ec != std::errc{} || ptr != end || pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

runtime_pid_status read_runtime_pid_status(const std::filesystem::path& pid_path,
                                           const runtime_process_probe& probe) {
    std::ifstream file(pid_path);
    if (!file.is_open()) {
        return {.state = runtime_pid_state::missing, .pid = std::nullopt};
    }

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto pid = parse_runtime_pid(text);
    if (!pid) {
        return {.state = runtime_pid_state::invalid, .pid = std::nullopt};
    }
    if (probe && probe(*pid)) {
        return {.state = runtime_pid_state::running, .pid = *pid};
    }
    return {.state = runtime_pid_state::stale, .pid = *pid};
}

bool write_runtime_pid_file(const std::filesystem::path& pid_path, int pid,
                            std::string* error) {
    std::error_code ec;
    if (!pid_path.parent_path().empty()) {
        std::filesystem::create_directories(pid_path.parent_path(), ec);
        if (ec) {
            if (error != nullptr) {
                *error = ec.message();
            }
            return false;
        }
    }

    std::ofstream file(pid_path, std::ios::trunc);
    if (!file) {
        if (error != nullptr) {
            *error = "could not open pid file";
        }
        return false;
    }
    file << pid << '\n';
    file.close();
    if (!file) {
        if (error != nullptr) {
            *error = "could not finish writing pid file";
        }
        return false;
    }
    return true;
}

void remove_runtime_files(const std::filesystem::path& pid_path,
                          const std::filesystem::path& status_path) {
    std::error_code ec;
    std::filesystem::remove(pid_path, ec);
    ec.clear();
    std::filesystem::remove(status_path, ec);
    ec.clear();
    std::filesystem::remove(tmp_status_path(status_path), ec);
}

runtime_claim_result claim_runtime_files(const std::filesystem::path& pid_path,
                                         const std::filesystem::path& status_path,
                                         int current_pid,
                                         const runtime_process_probe& probe) {
    runtime_claim_result result;
    result.existing = read_runtime_pid_status(pid_path, probe);
    if (result.existing.state == runtime_pid_state::running) {
        return result;
    }

    remove_runtime_files(pid_path, status_path);
    if (!write_runtime_pid_file(pid_path, current_pid, &result.error)) {
        return result;
    }
    result.claimed = true;
    return result;
}

int current_process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

bool is_process_running(int pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD exit_code = 0;
    const bool ok = GetExitCodeProcess(handle, &exit_code) != 0;
    CloseHandle(handle);
    return ok && exit_code == STILL_ACTIVE;
#else
    errno = 0;
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno == EPERM;
#endif
}

bool request_process_stop(int pid) {
    if (pid <= 0) {
        return false;
    }
#ifdef _WIN32
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    const bool ok = TerminateProcess(handle, 0) != 0;
    CloseHandle(handle);
    return ok;
#else
    errno = 0;
    return kill(static_cast<pid_t>(pid), SIGTERM) == 0 || errno == ESRCH;
#endif
}

bool wait_for_process_exit(int pid, std::chrono::milliseconds timeout,
                           std::chrono::milliseconds poll_interval) {
    auto waited = std::chrono::milliseconds(0);
    while (waited < timeout) {
        std::this_thread::sleep_for(poll_interval);
        waited += poll_interval;
        if (!is_process_running(pid)) {
            return true;
        }
    }
    return !is_process_running(pid);
}

}  // namespace mirage
