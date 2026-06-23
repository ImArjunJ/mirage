#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/runtime_process.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << text;
}

}  // namespace

int main() {
    bool ok = true;

    ok &= expect(mirage::parse_runtime_pid("42").value_or(0) == 42, "plain pid parse failed");
    ok &= expect(mirage::parse_runtime_pid("  42\n").value_or(0) == 42,
                 "trimmed pid parse failed");
    ok &= expect(!mirage::parse_runtime_pid("").has_value(), "empty pid parsed");
    ok &= expect(!mirage::parse_runtime_pid("0").has_value(), "zero pid parsed");
    ok &= expect(!mirage::parse_runtime_pid("-1").has_value(), "negative pid parsed");
    ok &= expect(!mirage::parse_runtime_pid("42x").has_value(), "trailing pid text parsed");
    ok &= expect(!mirage::parse_runtime_pid("999999999999999999999999").has_value(),
                 "overflow pid parsed");

    const auto root = std::filesystem::temp_directory_path() /
                      ("mirage-runtime-process-test-" +
                       std::to_string(mirage::current_process_id()));
    const auto pid_path = root / "state" / "mirage.pid";
    const auto status_path = root / "state" / "status.json";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    auto never_running = [](int) { return false; };
    auto only_42_running = [](int pid) { return pid == 42; };

    auto status = mirage::read_runtime_pid_status(pid_path, never_running);
    ok &= expect(status.state == mirage::runtime_pid_state::missing, "missing status mismatch");

    write_text(pid_path, "not-a-pid");
    status = mirage::read_runtime_pid_status(pid_path, never_running);
    ok &= expect(status.state == mirage::runtime_pid_state::invalid, "invalid status mismatch");
    ok &= expect(!status.pid.has_value(), "invalid status kept pid");

    write_text(pid_path, "99\n");
    status = mirage::read_runtime_pid_status(pid_path, never_running);
    ok &= expect(status.state == mirage::runtime_pid_state::stale, "stale status mismatch");
    ok &= expect(status.pid.value_or(0) == 99, "stale pid mismatch");

    write_text(pid_path, "42\n");
    status = mirage::read_runtime_pid_status(pid_path, only_42_running);
    ok &= expect(status.state == mirage::runtime_pid_state::running, "running status mismatch");
    ok &= expect(status.pid.value_or(0) == 42, "running pid mismatch");

    write_text(status_path, "{\"old\":true}");
    auto claim = mirage::claim_runtime_files(pid_path, status_path, 7, only_42_running);
    ok &= expect(!claim.claimed, "running pid was claimed");
    ok &= expect(claim.existing.state == mirage::runtime_pid_state::running,
                 "running claim existing status mismatch");
    ok &= expect(read_text(pid_path) == "42\n", "running claim rewrote pid");
    ok &= expect(std::filesystem::exists(status_path), "running claim removed status");

    write_text(pid_path, "99\n");
    write_text(status_path, "{\"old\":true}");
    write_text(status_path.string() + ".tmp", "tmp");
    claim = mirage::claim_runtime_files(pid_path, status_path, 123, never_running);
    ok &= expect(claim.claimed, "stale pid was not claimed");
    ok &= expect(claim.existing.state == mirage::runtime_pid_state::stale,
                 "stale claim existing status mismatch");
    ok &= expect(read_text(pid_path) == "123\n", "stale claim did not write new pid");
    ok &= expect(!std::filesystem::exists(status_path), "stale claim kept status");
    ok &= expect(!std::filesystem::exists(status_path.string() + ".tmp"),
                 "stale claim kept temp status");

    write_text(pid_path, "123\n");
    write_text(status_path, "{}");
    write_text(status_path.string() + ".tmp", "{}");
    mirage::remove_runtime_files(pid_path, status_path);
    ok &= expect(!std::filesystem::exists(pid_path), "remove kept pid");
    ok &= expect(!std::filesystem::exists(status_path), "remove kept status");
    ok &= expect(!std::filesystem::exists(status_path.string() + ".tmp"),
                 "remove kept temp status");

    std::filesystem::remove_all(root, ec);
    return ok ? 0 : 1;
}
