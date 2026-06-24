#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/cli_options.hpp"
#include "core/core.hpp"
#include "core/doctor_checks.hpp"
#include "core/log.hpp"
#include "core/port_probe.hpp"
#include "core/receiver_adapter.hpp"
#include "core/receiver_session.hpp"
#include "core/runtime_paths.hpp"
#include "core/runtime_process.hpp"
#include "core/runtime_status.hpp"
#include "core/status_report.hpp"
#include "crypto/crypto.hpp"
#include "discovery/discovery.hpp"
#include "io/event_loop.hpp"
#include "protocols/receiver_sources.hpp"
namespace {
volatile std::sig_atomic_t signal_received = 0;
void signal_handler(int signal) {
    signal_received = signal;
}

void print_help() {
    std::println(stderr, "mirage - local-network receiver");
    std::println(stderr, "");
    std::println(stderr, "usage:");
    std::println(stderr, "  mirage                      run in foreground");
    std::println(stderr, "  mirage --daemon             run in background");
    std::println(stderr, "  mirage stop                 stop background instance");
    std::println(stderr, "  mirage status               show current state");
    std::println(stderr, "  mirage status -v            show detailed state");
    std::println(stderr, "  mirage service <command>    manage windows service");
    std::println(stderr, "  mirage paths                show runtime file paths");
    std::println(stderr, "  mirage doctor               check startup configuration");
    std::println(stderr, "");
    std::println(stderr, "options:");
    std::println(stderr, "  --name <name>       device name (default: Mirage)");
    std::println(stderr, "  --port <port>       airplay port (default: 7000)");
    std::println(stderr, "  --cast-port <port>  cast port (default: 8009)");
    std::println(stderr, "  --miracast-port <port>");
    std::println(stderr, "                      miracast port (default: 7236)");
    std::println(stderr, "  --no-airplay        disable airplay");
    std::println(stderr, "  --cast              enable experimental google cast probe adapter");
    std::println(stderr, "  --miracast          enable experimental miracast capability listener");
    std::println(stderr, "  --no-cast           disable google cast if enabled by config");
    std::println(stderr, "  --no-miracast       disable miracast if enabled by config");
    std::println(stderr, "  --no-mdns           disable built-in mdns broadcaster");
    std::println(stderr, "  --diagnostics       show compact session summaries");
    std::println(stderr, "  --verbose           show more output");
    std::println(stderr, "  --debug             show protocol events");
    std::println(stderr, "  --trace             show packet-level logs");
    std::println(stderr, "  --config <file>     config file path");
    std::println(stderr, "                      default: per-user mirage/config.conf if present");
    std::println(stderr, "  --identity-key <file>");
    std::println(stderr, "                      persistent receiver identity key");
    std::println(stderr, "  --version           print version");
    std::println(stderr, "  --help              show this help");
}

void setup_logging(bool verbose, bool debug, bool trace, bool diagnostics) {
    mirage::log::diagnostics_enabled = diagnostics;
    if (trace) {
        mirage::log::min_level = mirage::log::level::trace;
    } else if (debug) {
        mirage::log::min_level = mirage::log::level::debug;
    } else if (verbose) {
        mirage::log::min_level = mirage::log::level::info;
    } else {
        mirage::log::min_level = mirage::log::level::user;
    }
}

void drain_shutdown_work(mirage::io::io_context& ctx) {
    for (int i = 0; i < 32; ++i) {
        ctx.run_for(std::chrono::milliseconds(2));
    }
}

std::vector<std::string_view> argv_view(int argc, char* argv[], int first) {
    std::vector<std::string_view> args;
    for (int i = first; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return args;
}

void print_cli_error(const mirage::cli_error& error) {
    std::println(stderr, "{}", error.message);
}

std::filesystem::path state_dir() {
    return mirage::runtime_state_dir(mirage::current_runtime_path_environment());
}

std::filesystem::path default_config_file_path() {
    return mirage::runtime_default_config_file_path(mirage::current_runtime_path_environment());
}

std::filesystem::path pid_file_path() {
    return mirage::runtime_pid_file_path(mirage::current_runtime_path_environment());
}

std::filesystem::path status_file_path() {
    return mirage::runtime_status_file_path(mirage::current_runtime_path_environment());
}

std::filesystem::path identity_key_path(const mirage::config& cfg) {
    return mirage::runtime_identity_key_path(cfg, mirage::current_runtime_path_environment());
}

void log_receiver_identity_key_result(const std::filesystem::path& path,
                                      const mirage::receiver_identity_keypair& identity) {
    for (const auto& warning : identity.warnings) {
        mirage::log::warn("{}", warning);
    }
    switch (identity.source) {
        case mirage::receiver_identity_key_source::loaded:
            mirage::log::info("loaded persistent receiver identity: {}", path.string());
            break;
        case mirage::receiver_identity_key_source::created:
            mirage::log::info("created persistent receiver identity: {}", path.string());
            break;
        case mirage::receiver_identity_key_source::transient:
            mirage::log::warn("using transient receiver identity for this run");
            break;
    }
}

std::string inspect_port_command(uint16_t port) {
#ifdef _WIN32
    return std::format("netstat -ano | findstr :{}", port);
#else
    return std::format("lsof -i :{}", port);
#endif
}

std::optional<std::string> environment_value(std::string_view name) {
    auto key = std::string(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr || value[0] == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

void print_doctor_check(const mirage::doctor::check_result& check, bool& ok) {
    switch (check.level) {
        case mirage::doctor::check_level::ok:
            std::println("  {}: {}", check.name, check.detail);
            break;
        case mirage::doctor::check_level::note:
            std::println("  {}: {}", check.name, check.detail);
            if (!check.fix.empty()) {
                std::println("  note: {}", check.fix);
            }
            break;
        case mirage::doctor::check_level::error:
            ok = false;
            std::println("  {}: {}", check.name, check.detail);
            if (!check.fix.empty()) {
                std::println("check: {}", check.fix);
            }
            break;
    }
}

int handle_paths(int argc, char* argv[]) {
    mirage::config cfg;
    auto args = argv_view(argc, argv, 2);
    for (auto arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::println("usage: mirage paths [--config <file>]");
            return 0;
        }
    }
    auto parsed = mirage::parse_paths_cli_options(args, default_config_file_path());
    if (!parsed) {
        print_cli_error(parsed.error());
        return parsed.error().exit_code();
    }
    auto config_path = parsed->config_path;

    const bool config_exists = std::filesystem::exists(config_path);
    if (config_exists) {
        auto loaded = mirage::config::load_from_file(config_path.string());
        if (loaded) {
            cfg = *loaded;
        } else if (parsed->explicit_config) {
            std::println(stderr, "failed to load config: {}", loaded.error().message);
            return 1;
        }
    } else if (parsed->explicit_config) {
        std::println(stderr, "config file not found: {}", config_path.string());
        return 1;
    }

    std::println("config: {}{}", config_path.string(), config_exists ? "" : " (not found)");
    std::println("state: {}", state_dir().string());
    std::println("pid: {}", pid_file_path().string());
    std::println("status: {}", status_file_path().string());
    std::println("identity key: {}", identity_key_path(cfg).string());
    return 0;
}

std::string capability_summary(const mirage::receiver_source_capabilities& capabilities) {
    std::string summary;
    auto append = [&](bool enabled, std::string_view label) {
        if (!enabled) {
            return;
        }
        if (!summary.empty()) {
            summary += "/";
        }
        summary += label;
    };
    append(capabilities.audio, "audio");
    append(capabilities.video, "video");
    append(capabilities.app_lifecycle, "apps");
    append(capabilities.media_control, "media");
    append(capabilities.remote_control, "remote");
    append(capabilities.metadata, "metadata");
    return summary.empty() ? "control" : summary;
}

int handle_doctor(int argc, char* argv[]) {
    auto args = argv_view(argc, argv, 2);
    for (auto arg : args) {
        if (arg == "--help" || arg == "-h") {
            std::println("usage: mirage doctor [runtime options]");
            std::println("");
            std::println("checks config, enabled receivers, paths, ports, and network state.");
            return 0;
        }
    }

    auto parsed = mirage::parse_runtime_cli_options(args, default_config_file_path());
    if (!parsed) {
        print_cli_error(parsed.error());
        return parsed.error().exit_code();
    }
    const auto& cfg = parsed->cfg;

    const auto sources = mirage::protocols::make_receiver_source_descriptors(cfg);
    bool ok = true;
    std::println("mirage doctor");
    std::println("config: {}{}", parsed->config_path.string(),
                 parsed->config_exists ? "" : " (not found)");
    std::println("state: {}", state_dir().string());
    std::println("identity key: {}", identity_key_path(cfg).string());
    std::println("mode: {}", parsed->daemon_mode ? "daemon" : "foreground");
    std::println("mdns: {}", parsed->no_mdns ? "disabled" : "enabled");
    std::println("protocols:");

    for (const auto& source : sources) {
        std::string line = std::format("  {}: {}", mirage::protocol_id(source.id),
                                       source.enabled ? "enabled" : "disabled");
        if (source.port != 0) {
            line += std::format(", port {}", source.port);
        }
        if (!source.capabilities.transport.empty()) {
            line += std::format(", {}", source.capabilities.transport);
        }
        if (source.enabled) {
            line += std::format(", {}", capability_summary(source.capabilities));
        }
        if (source.experimental) {
            line += ", experimental";
        }
        if (!source.detail.empty()) {
            line += std::format(", {}", source.detail);
        }
        std::println("{}", line);
    }

    auto enabled_count =
        std::ranges::count_if(sources, [](const auto& source) { return source.enabled; });
    if (enabled_count == 0) {
        ok = false;
        std::println("check: no receiver protocols enabled");
    }

    bool duplicate_ports = false;
    for (size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].enabled || sources[i].port == 0) {
            continue;
        }
        for (size_t j = i + 1; j < sources.size(); ++j) {
            if (sources[j].enabled && sources[j].port == sources[i].port) {
                ok = false;
                duplicate_ports = true;
                std::println("check: {} and {} share port {}", mirage::protocol_id(sources[i].id),
                             mirage::protocol_id(sources[j].id), sources[i].port);
            }
        }
    }

    bool checked_ports = false;
    bool ports_available = true;
    for (const auto& source : sources) {
        if (!source.enabled || source.port == 0) {
            continue;
        }
        checked_ports = true;
        auto probe = mirage::probe_tcp_port_available(source.port);
        if (!probe.available) {
            ok = false;
            ports_available = false;
            std::println("check: {} port {} unavailable ({})", mirage::protocol_id(source.id),
                         source.port, probe.message);
            std::println("       inspect with: {}", inspect_port_command(source.port));
        }
    }
    if (checked_ports && ports_available && !duplicate_ports) {
        std::println("ports: available");
    }

    auto interfaces = mirage::discovery::enumerate_interfaces();
    if (!interfaces) {
        ok = false;
        std::println("network: {}", interfaces.error().message);
    } else {
        std::optional<mirage::discovery::network_interface> selected;
        for (const auto& iface : *interfaces) {
            if (!iface.is_loopback && iface.is_up && !iface.mac_address.empty()) {
                selected = iface;
                break;
            }
        }
        if (selected) {
            std::string address;
            for (const auto& addr : selected->addresses) {
                if (addr.is_v4()) {
                    address = addr.to_string();
                    break;
                }
            }
            if (address.empty()) {
                std::println("network: {} up, no ipv4 address", selected->name);
            } else {
                std::println("network: {} {}", selected->name, address);
            }
        } else {
            ok = false;
            std::println("network: no usable non-loopback interface");
        }
    }

    std::println("dependencies:");
    for (const auto& check : mirage::doctor::collect_runtime_checks()) {
        print_doctor_check(check, ok);
    }

    std::println("assets:");
    for (const auto& check : mirage::doctor::collect_asset_checks()) {
        print_doctor_check(check, ok);
    }

    std::println("backends:");
    for (const auto& check : mirage::doctor::collect_backend_hints(environment_value)) {
        print_doctor_check(check, ok);
    }

    std::println("result: {}", ok ? "ready" : "attention");
    return ok ? 0 : 1;
}

void clear_runtime_files();

int handle_stop() {
    auto status = mirage::read_runtime_pid_status(pid_file_path(), mirage::is_process_running);
    if (status.state != mirage::runtime_pid_state::running || !status.pid) {
        clear_runtime_files();
        std::println(stderr, "mirage is not running.");
        return 1;
    }

    if (!mirage::request_process_stop(*status.pid)) {
        std::println(stderr, "could not stop mirage (pid {}).", *status.pid);
        return 1;
    }
    if (mirage::wait_for_process_exit(*status.pid, std::chrono::seconds(3))) {
        mirage::remove_runtime_files(pid_file_path(), status_file_path());
        std::println(stderr, "mirage stopped.");
        return 0;
    }
    std::println(stderr, "mirage did not stop within 3 seconds (pid {}).", *status.pid);
    return 1;
}

int handle_status(bool verbose) {
    auto status = mirage::read_runtime_pid_status(pid_file_path(), mirage::is_process_running);
    if (status.state != mirage::runtime_pid_state::running || !status.pid) {
        clear_runtime_files();
        std::println(stderr, "mirage is not running.");
        return 0;
    }
    auto path = status_file_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        std::println(stderr, "mirage is running (pid {})", *status.pid);
        return 0;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto summary = mirage::parse_status_summary(json);
    std::print(stderr, "{}", mirage::render_status_summary_text(summary, *status.pid, verbose));
    return 0;
}

void print_receiver_start_error(mirage::protocol id, uint16_t port,
                                const mirage::mirage_error& error) {
    switch (id) {
        case mirage::protocol::airplay:
            std::println(stderr, "could not start airplay on port {}.", port);
            std::println(stderr, "  {}", error.message);
            std::println(stderr,
                         "  the port may be in use. try --port <port> to use a different one,");
            std::println(stderr, "  or run: {}", inspect_port_command(port));
            break;
        case mirage::protocol::cast:
            std::println(stderr, "could not start cast on port {}.", port);
            std::println(stderr, "  {}", error.message);
            std::println(stderr, "  the port may be in use. try a different port or check:");
            std::println(stderr, "  {}", inspect_port_command(port));
            break;
        case mirage::protocol::miracast:
            std::println(stderr, "could not start miracast on port {}.", port);
            std::println(stderr, "  {}", error.message);
            std::println(stderr, "  the port may be in use. try a different port or check:");
            std::println(stderr, "  {}", inspect_port_command(port));
            break;
    }
}

bool write_runtime_pid_or_report(int pid) {
    std::string error;
    if (mirage::write_runtime_pid_file(pid_file_path(), pid, &error)) {
        return true;
    }
    std::println(stderr, "could not write pid file: {}{}", pid_file_path().string(),
                 error.empty() ? "" : std::format(" ({})", error));
    return false;
}

void clear_runtime_files() {
    mirage::remove_runtime_files(pid_file_path(), status_file_path());
}

bool claim_runtime_for_process(int pid) {
    auto claim = mirage::claim_runtime_files(pid_file_path(), status_file_path(), pid,
                                             mirage::is_process_running);
    if (claim.claimed) {
        return true;
    }
    if (claim.existing.state == mirage::runtime_pid_state::running && claim.existing.pid) {
        std::println(stderr, "mirage is already running (pid {}).", *claim.existing.pid);
    } else {
        std::println(stderr, "could not write pid file: {}{}", pid_file_path().string(),
                     claim.error.empty() ? "" : std::format(" ({})", claim.error));
    }
    return false;
}

bool clear_stale_runtime_before_daemon() {
    auto status = mirage::read_runtime_pid_status(pid_file_path(), mirage::is_process_running);
    if (status.state == mirage::runtime_pid_state::running && status.pid) {
        std::println(stderr, "mirage is already running (pid {}).", *status.pid);
        return false;
    }
    if (status.state != mirage::runtime_pid_state::running) {
        clear_runtime_files();
    }
    return true;
}

using started_callback = void (*)();

int run_receiver(const mirage::runtime_cli_options& parsed, bool owns_runtime_files,
                 bool show_stop_hint, started_callback on_started = nullptr);

#ifdef _WIN32
std::wstring widen_utf8(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input_size = static_cast<int>(input.size());
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), input_size, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), input_size, output.data(),
                        size);
    return output;
}

std::string narrow_utf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int input_size = static_cast<int>(input.size());
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), input_size, output.data(), size, nullptr,
                        nullptr);
    return output;
}

std::string windows_error_message(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size =
        FormatMessageW(flags, nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (size == 0 || buffer == nullptr) {
        return std::format("windows error {}", error);
    }
    std::wstring_view message(buffer, size);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                                message.back() == L' ' || message.back() == L'\t')) {
        message.remove_suffix(1);
    }
    auto text = narrow_utf8(message);
    LocalFree(buffer);
    return text.empty() ? std::format("windows error {}", error) : text;
}

std::optional<std::wstring> windows_executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD size =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return std::nullopt;
        }
        if (static_cast<size_t>(size) < buffer.size()) {
            buffer.resize(static_cast<size_t>(size));
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring quote_windows_arg(std::wstring_view arg) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

bool redirect_windows_output_to_log() {
    auto dir = state_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return false;
    }
    auto log_path = dir / "mirage.log";
    FILE* stderr_file = nullptr;
    FILE* stdout_file = nullptr;
    _wfreopen_s(&stderr_file, log_path.c_str(), L"a", stderr);
    _wfreopen_s(&stdout_file, log_path.c_str(), L"a", stdout);
    return stderr_file != nullptr || stdout_file != nullptr;
}

bool daemonize_child() {
    if (!clear_stale_runtime_before_daemon()) {
        return false;
    }
    FreeConsole();
    redirect_windows_output_to_log();
    if (!write_runtime_pid_or_report(mirage::current_process_id())) {
        return false;
    }
    std::println(stderr, "mirage started as background process (pid {})",
                 mirage::current_process_id());
    return true;
}

bool daemonize_parent(int argc, char* argv[]) {
    if (!clear_stale_runtime_before_daemon()) {
        return false;
    }
    auto exe = windows_executable_path();
    if (!exe) {
        std::println(stderr, "could not find mirage executable path: {}",
                     windows_error_message(GetLastError()));
        return false;
    }

    std::wstring command = quote_windows_arg(*exe);
    bool replaced_daemon_flag = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (!replaced_daemon_flag && (arg == "--daemon" || arg == "-d")) {
            command += L" ";
            command += quote_windows_arg(L"--background-child");
            replaced_daemon_flag = true;
            continue;
        }
        command += L" ";
        command += quote_windows_arg(widen_utf8(arg));
    }
    if (!replaced_daemon_flag) {
        command += L" ";
        command += quote_windows_arg(L"--background-child");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const DWORD flags = DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        nullptr, &startup, &process)) {
        std::println(stderr, "could not start mirage in background: {}",
                     windows_error_message(GetLastError()));
        return false;
    }
    const int child_pid = static_cast<int>(process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (!write_runtime_pid_or_report(child_pid)) {
        return false;
    }
    std::println(stderr, "mirage started (pid {})", child_pid);
    return true;
}

constexpr wchar_t windows_service_name[] = L"mirage";
constexpr wchar_t windows_service_display_name[] = L"Mirage Receiver";

struct service_handle {
    SC_HANDLE value = nullptr;

    service_handle() = default;
    explicit service_handle(SC_HANDLE handle) : value(handle) {}
    service_handle(const service_handle&) = delete;
    service_handle& operator=(const service_handle&) = delete;
    service_handle(service_handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    service_handle& operator=(service_handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value, nullptr));
        }
        return *this;
    }
    ~service_handle() { reset(); }

    void reset(SC_HANDLE handle = nullptr) {
        if (value != nullptr) {
            CloseServiceHandle(value);
        }
        value = handle;
    }
};

std::vector<std::string> windows_service_runtime_args;
SERVICE_STATUS_HANDLE windows_service_status_handle = nullptr;
SERVICE_STATUS windows_service_status{};
DWORD windows_service_checkpoint = 1;

std::vector<std::string_view> service_arg_views(const std::vector<std::string>& args) {
    std::vector<std::string_view> views;
    views.reserve(args.size());
    for (const auto& arg : args) {
        views.emplace_back(arg);
    }
    return views;
}

void print_windows_service_usage() {
    std::println(stderr, "usage:");
    std::println(stderr, "  mirage service install [runtime options]");
    std::println(stderr, "  mirage service start");
    std::println(stderr, "  mirage service stop");
    std::println(stderr, "  mirage service status");
    std::println(stderr, "  mirage service uninstall");
    std::println(stderr, "");
    std::println(stderr, "service install stores the runtime options in the windows service.");
    std::println(stderr, "run from an elevated powershell when installing or uninstalling.");
}

bool runtime_args_include_daemon(std::span<const std::string_view> args) {
    return std::ranges::any_of(args, [](std::string_view arg) {
        return arg == "--daemon" || arg == "-d" || arg == "--background-child";
    });
}

std::wstring service_binary_command(std::span<const std::string_view> runtime_args) {
    auto exe = windows_executable_path();
    if (!exe) {
        return {};
    }
    std::wstring command = quote_windows_arg(*exe);
    command += L" ";
    command += quote_windows_arg(L"service");
    command += L" ";
    command += quote_windows_arg(L"run");
    for (auto arg : runtime_args) {
        command += L" ";
        command += quote_windows_arg(widen_utf8(arg));
    }
    return command;
}

service_handle open_service_control_manager(DWORD access) {
    return service_handle(OpenSCManagerW(nullptr, nullptr, access));
}

service_handle open_mirage_service(SC_HANDLE scm, DWORD access) {
    return service_handle(OpenServiceW(scm, windows_service_name, access));
}

std::string service_state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED:
            return "stopped";
        case SERVICE_START_PENDING:
            return "starting";
        case SERVICE_STOP_PENDING:
            return "stopping";
        case SERVICE_RUNNING:
            return "running";
        case SERVICE_CONTINUE_PENDING:
            return "continue pending";
        case SERVICE_PAUSE_PENDING:
            return "pause pending";
        case SERVICE_PAUSED:
            return "paused";
        default:
            return std::format("unknown ({})", state);
    }
}

std::optional<SERVICE_STATUS_PROCESS> query_service_status(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                              sizeof(status), &bytes_needed)) {
        return std::nullopt;
    }
    return status;
}

bool wait_for_service_state(SC_HANDLE service, DWORD wanted_state, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = query_service_status(service);
        if (!status) {
            return false;
        }
        if (status->dwCurrentState == wanted_state) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    auto status = query_service_status(service);
    return status && status->dwCurrentState == wanted_state;
}

bool stop_windows_service_handle(SC_HANDLE service, bool quiet_if_stopped) {
    auto status = query_service_status(service);
    if (!status) {
        std::println(stderr, "could not query windows service: {}",
                     windows_error_message(GetLastError()));
        return false;
    }
    if (status->dwCurrentState == SERVICE_STOPPED) {
        if (!quiet_if_stopped) {
            std::println(stderr, "mirage service is already stopped.");
        }
        return true;
    }

    SERVICE_STATUS stop_status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &stop_status)) {
        const auto error = GetLastError();
        if (error == ERROR_SERVICE_NOT_ACTIVE) {
            return true;
        }
        std::println(stderr, "could not stop windows service: {}", windows_error_message(error));
        return false;
    }
    if (!wait_for_service_state(service, SERVICE_STOPPED, std::chrono::seconds(15))) {
        std::println(stderr, "mirage service did not stop within 15 seconds.");
        return false;
    }
    if (!quiet_if_stopped) {
        std::println(stderr, "mirage service stopped.");
    }
    return true;
}

int handle_windows_service_install(std::span<const std::string_view> runtime_args) {
    if (runtime_args_include_daemon(runtime_args)) {
        std::println(stderr, "service install does not accept --daemon.");
        return 2;
    }
    auto parsed = mirage::parse_runtime_cli_options(runtime_args, default_config_file_path());
    if (!parsed) {
        print_cli_error(parsed.error());
        return parsed.error().exit_code();
    }

    auto binary_command = service_binary_command(runtime_args);
    if (binary_command.empty()) {
        std::println(stderr, "could not find mirage executable path: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }

    auto scm = open_service_control_manager(SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (scm.value == nullptr) {
        const auto error = GetLastError();
        std::println(stderr, "could not open windows service manager: {}",
                     windows_error_message(error));
        if (error == ERROR_ACCESS_DENIED) {
            std::println(stderr, "run this command from an elevated powershell.");
        }
        return 1;
    }

    auto service = service_handle(CreateServiceW(
        scm.value, windows_service_name, windows_service_display_name, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binary_command.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr));
    if (service.value != nullptr) {
        std::println(stderr, "mirage service installed.");
        return 0;
    }

    const auto create_error = GetLastError();
    if (create_error != ERROR_SERVICE_EXISTS) {
        std::println(stderr, "could not install windows service: {}",
                     windows_error_message(create_error));
        if (create_error == ERROR_ACCESS_DENIED) {
            std::println(stderr, "run this command from an elevated powershell.");
        }
        return 1;
    }

    service = open_mirage_service(scm.value, SERVICE_CHANGE_CONFIG);
    if (service.value == nullptr) {
        std::println(stderr, "could not open existing windows service: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    if (!ChangeServiceConfigW(service.value, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                              SERVICE_ERROR_NORMAL, binary_command.c_str(), nullptr, nullptr,
                              nullptr, nullptr, nullptr, windows_service_display_name)) {
        std::println(stderr, "could not update windows service: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    std::println(stderr, "mirage service updated.");
    return 0;
}

int handle_windows_service_start() {
    auto scm = open_service_control_manager(SC_MANAGER_CONNECT);
    if (scm.value == nullptr) {
        std::println(stderr, "could not open windows service manager: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    auto service = open_mirage_service(scm.value, SERVICE_START | SERVICE_QUERY_STATUS);
    if (service.value == nullptr) {
        std::println(stderr, "mirage service is not installed.");
        return 1;
    }
    if (!StartServiceW(service.value, 0, nullptr)) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) {
            std::println(stderr, "could not start windows service: {}",
                         windows_error_message(error));
            return 1;
        }
    }
    if (!wait_for_service_state(service.value, SERVICE_RUNNING, std::chrono::seconds(15))) {
        std::println(stderr, "mirage service did not report running within 15 seconds.");
        return 1;
    }
    std::println(stderr, "mirage service started.");
    return 0;
}

int handle_windows_service_stop() {
    auto scm = open_service_control_manager(SC_MANAGER_CONNECT);
    if (scm.value == nullptr) {
        std::println(stderr, "could not open windows service manager: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    auto service = open_mirage_service(scm.value, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service.value == nullptr) {
        std::println(stderr, "mirage service is not installed.");
        return 1;
    }
    return stop_windows_service_handle(service.value, false) ? 0 : 1;
}

int handle_windows_service_status() {
    auto scm = open_service_control_manager(SC_MANAGER_CONNECT);
    if (scm.value == nullptr) {
        std::println(stderr, "could not open windows service manager: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    auto service = open_mirage_service(scm.value, SERVICE_QUERY_STATUS);
    if (service.value == nullptr) {
        std::println(stderr, "mirage service is not installed.");
        return 1;
    }
    auto status = query_service_status(service.value);
    if (!status) {
        std::println(stderr, "could not query windows service: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    std::println(stderr, "mirage service: {}", service_state_name(status->dwCurrentState));
    if (status->dwProcessId != 0) {
        std::println(stderr, "pid: {}", status->dwProcessId);
    }
    return 0;
}

int handle_windows_service_uninstall() {
    auto scm = open_service_control_manager(SC_MANAGER_CONNECT);
    if (scm.value == nullptr) {
        std::println(stderr, "could not open windows service manager: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    auto service = open_mirage_service(scm.value, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (service.value == nullptr) {
        std::println(stderr, "mirage service is not installed.");
        return 0;
    }
    if (!stop_windows_service_handle(service.value, true)) {
        return 1;
    }
    if (!DeleteService(service.value)) {
        std::println(stderr, "could not uninstall windows service: {}",
                     windows_error_message(GetLastError()));
        return 1;
    }
    std::println(stderr, "mirage service uninstalled.");
    return 0;
}

void set_windows_service_status(DWORD state, DWORD win32_exit_code = NO_ERROR,
                                DWORD wait_hint_ms = 0) {
    if (windows_service_status_handle == nullptr) {
        return;
    }
    windows_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    windows_service_status.dwCurrentState = state;
    windows_service_status.dwWin32ExitCode = win32_exit_code;
    windows_service_status.dwServiceSpecificExitCode = 0;
    windows_service_status.dwWaitHint = wait_hint_ms;
    windows_service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        windows_service_status.dwCheckPoint = windows_service_checkpoint++;
    } else {
        windows_service_status.dwCheckPoint = 0;
    }
    SetServiceStatus(windows_service_status_handle, &windows_service_status);
}

void windows_service_started() {
    set_windows_service_status(SERVICE_RUNNING);
}

void WINAPI windows_service_control_handler(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        set_windows_service_status(SERVICE_STOP_PENDING, NO_ERROR, 15000);
        signal_received = SIGTERM;
    }
}

void WINAPI windows_service_main(DWORD, LPWSTR*) {
    windows_service_status_handle =
        RegisterServiceCtrlHandlerW(windows_service_name, windows_service_control_handler);
    if (windows_service_status_handle == nullptr) {
        return;
    }
    windows_service_checkpoint = 1;
    set_windows_service_status(SERVICE_START_PENDING, NO_ERROR, 15000);
    redirect_windows_output_to_log();

    auto views = service_arg_views(windows_service_runtime_args);
    auto parsed = mirage::parse_runtime_cli_options(views, default_config_file_path());
    if (!parsed) {
        print_cli_error(parsed.error());
        set_windows_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    if (parsed->daemon_mode) {
        std::println(stderr, "service run does not accept --daemon.");
        set_windows_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }

    setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);
    if (!claim_runtime_for_process(mirage::current_process_id())) {
        set_windows_service_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    const int exit_code = run_receiver(*parsed, true, false, windows_service_started);
    set_windows_service_status(SERVICE_STOPPED,
                               exit_code == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

int run_windows_service_dispatcher(std::vector<std::string> runtime_args) {
    windows_service_runtime_args = std::move(runtime_args);
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(windows_service_name), windows_service_main},
        {nullptr, nullptr},
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        const auto error = GetLastError();
        std::println(stderr, "could not connect to windows service manager: {}",
                     windows_error_message(error));
        std::println(stderr, "use 'mirage service start' to start the installed service.");
        return 1;
    }
    return 0;
}

int handle_service_command(int argc, char* argv[]) {
    if (argc < 3 || std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h") {
        print_windows_service_usage();
        return 0;
    }

    const std::string_view command(argv[2]);
    if (command == "install") {
        auto runtime_args = argv_view(argc, argv, 3);
        return handle_windows_service_install(runtime_args);
    }
    if (command == "start") {
        return handle_windows_service_start();
    }
    if (command == "stop") {
        return handle_windows_service_stop();
    }
    if (command == "status") {
        return handle_windows_service_status();
    }
    if (command == "uninstall") {
        return handle_windows_service_uninstall();
    }
    if (command == "run") {
        std::vector<std::string> runtime_args;
        for (int i = 3; i < argc; ++i) {
            runtime_args.emplace_back(argv[i]);
        }
        return run_windows_service_dispatcher(std::move(runtime_args));
    }

    std::println(stderr, "unknown service command: {}", command);
    print_windows_service_usage();
    return 2;
}
#else
bool daemonize(pid_t& child_pid) {
    if (!clear_stale_runtime_before_daemon()) {
        return false;
    }

    auto dir = state_dir();
    std::filesystem::create_directories(dir);

    child_pid = fork();
    if (child_pid < 0) {
        std::println(stderr, "failed to fork.");
        return false;
    }
    if (child_pid > 0) {
        if (!write_runtime_pid_or_report(static_cast<int>(child_pid))) {
            _exit(1);
        }
        std::println(stderr, "mirage started (pid {})", child_pid);
        _exit(0);
    }

    setsid();

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    open("/dev/null", O_RDONLY);

    auto log_path = dir / "mirage.log";
    int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        log_fd = open("/dev/null", O_WRONLY);
    }
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    if (log_fd > STDERR_FILENO) {
        close(log_fd);
    }

    if (!write_runtime_pid_or_report(mirage::current_process_id())) {
        return false;
    }
    return true;
}
#endif

#ifndef _WIN32
int handle_service_command(int argc, char* argv[]) {
    if (argc < 3 || std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h") {
        std::println(stderr, "usage:");
        std::println(stderr, "  mirage service install [runtime options]");
        std::println(stderr, "  mirage service start");
        std::println(stderr, "  mirage service stop");
        std::println(stderr, "  mirage service status");
        std::println(stderr, "  mirage service uninstall");
        std::println(stderr, "");
        std::println(stderr, "windows service support is only available on windows.");
        return 0;
    }
    std::println(stderr, "windows service support is only available on windows.");
    return 1;
}
#endif

int run_receiver(const mirage::runtime_cli_options& parsed, bool owns_runtime_files,
                 bool show_stop_hint, started_callback on_started) {
    const auto& cfg = parsed.cfg;
    signal_received = 0;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    try {
        mirage::io::io_context ctx;
        auto receiver_identity_path = identity_key_path(cfg);
        auto receiver_identity =
            mirage::load_or_create_receiver_identity_keypair(receiver_identity_path);
        if (!receiver_identity) {
            std::println(stderr, "crypto error: could not load or generate receiver identity.");
            std::println(stderr, "  {}", receiver_identity.error().message);
            std::println(stderr, "  this is unusual -- check that openssl is installed correctly.");
            if (owns_runtime_files) {
                clear_runtime_files();
            }
            return 1;
        }
        log_receiver_identity_key_result(receiver_identity_path, *receiver_identity);
        auto receiver_public_key = receiver_identity->keypair.public_key();
        auto interfaces = mirage::discovery::enumerate_interfaces();
        if (!interfaces) {
            std::println(stderr, "no network interfaces found.");
            std::println(stderr, "  make sure wifi or ethernet is connected and up.");
            if (owns_runtime_files) {
                clear_runtime_files();
            }
            return 1;
        }
        std::string mac_address = "AA:BB:CC:DD:EE:FF";
        std::string local_ip;
        std::string iface_name;
        for (const auto& iface : *interfaces) {
            if (!iface.is_loopback && iface.is_up && !iface.mac_address.empty()) {
                mac_address = iface.mac_address;
                iface_name = iface.name;
                for (const auto& addr : iface.addresses) {
                    if (addr.is_v4()) {
                        local_ip = addr.to_string();
                        break;
                    }
                }
                mirage::log::info("using interface: {} ({}) {}", iface.name, mac_address, local_ip);
                break;
            }
        }

        auto receiver_sources = mirage::receiver_source_registry(
            mirage::protocols::make_receiver_source_descriptors(cfg));
        mirage::receiver_adapter_registry adapters(receiver_sources.all());

        std::optional<mirage::discovery::mdns_broadcaster> mdns;
        std::optional<mirage::discovery::mdns_service_publisher> mdns_publisher;
        mirage::discovery::disabled_service_publisher disabled_discovery;
        mirage::discovery::service_publisher* discovery = &disabled_discovery;
        if (!parsed.no_mdns) {
            mdns.emplace(ctx);
            mdns_publisher.emplace(*mdns);
            discovery = &*mdns_publisher;
            mirage::log::info("built-in mdns broadcaster enabled");
        }
        std::vector<std::unique_ptr<mirage::receiver_session>> receiver_sessions;
        const auto started_at =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        mirage::runtime_status_tracker status_tracker(
            status_file_path(), mirage::current_process_id(), cfg, local_ip, iface_name,
            receiver_identity_path, started_at, adapters, receiver_sources.all());
        const mirage::receiver_source_runtime receiver_runtime{
            .io_context = &ctx,
            .receiver_identity = &receiver_identity->keypair,
            .receiver_public_key = &receiver_public_key,
            .session_observer = &status_tracker,
            .device_name = cfg.device_name,
            .mac_address = mac_address,
            .hardware_decode = cfg.hardware_decode,
        };
        auto start_receiver_session = [&](std::unique_ptr<mirage::receiver_session> session) {
            auto id = session->id();
            auto port = session->port();
            auto started = session->start(adapters, *discovery);
            if (!started) {
                print_receiver_start_error(id, port, started.error());
                return;
            }

            auto task = session->run();
            receiver_sessions.push_back(std::move(session));
            mirage::io::co_spawn(ctx, std::move(task));
        };

        for (const auto& source : receiver_sources.all()) {
            if (!source.enabled) {
                continue;
            }
            auto session = source.create_session(receiver_runtime);
            if (!session) {
                adapters.mark_error(source.id, session.error().message);
                print_receiver_start_error(source.id, source.port, session.error());
                continue;
            }
            start_receiver_session(std::move(*session));
        }
        static_cast<void>(status_tracker.write());
        if (receiver_sessions.empty()) {
            if (receiver_sources.enabled().empty()) {
                std::println(stderr, "no receiver protocols are enabled.");
                std::println(stderr,
                             "  enable airplay, cast, or miracast with config or command-line "
                             "options.");
            } else {
                std::println(stderr, "no receiver protocols started.");
                std::println(stderr, "  check the startup errors above.");
            }
            if (mdns) {
                mdns->stop();
            }
            drain_shutdown_work(ctx);
            ctx.stop();
            if (owns_runtime_files) {
                clear_runtime_files();
            }
            return 1;
        }
        if (mdns) {
            mirage::io::co_spawn(ctx, mdns->run());
        }

        mirage::log::user("mirage started{}",
                          local_ip.empty() ? "" : std::format(" on {}", local_ip));
        for (const auto& source : receiver_sources.all()) {
            if (!source.enabled) {
                continue;
            }
            const auto* adapter = adapters.find(source.id);
            if (adapter != nullptr && adapter->state == mirage::receiver_adapter_state::error) {
                mirage::log::user("  {} failed: {}", mirage::protocol_id(source.id),
                                  adapter->detail);
                continue;
            }
            if (source.port != 0) {
                mirage::log::user("  {} on port {}", mirage::protocol_id(source.id), source.port);
            } else {
                mirage::log::user("  {} enabled", mirage::protocol_id(source.id));
            }
        }
        if (show_stop_hint) {
            mirage::log::user("  press ctrl+c to stop.");
        }
        if (on_started != nullptr) {
            on_started();
        }
        while (signal_received == 0) {
            ctx.run_for(std::chrono::milliseconds(100));
        }
        mirage::log::info("received signal {}, shutting down", static_cast<int>(signal_received));
        for (auto& session : receiver_sessions) {
            session->stop(adapters, *discovery);
        }
        discovery->withdraw_all();
        if (mdns) {
            mdns->stop();
        }
        drain_shutdown_work(ctx);
        ctx.stop();
    } catch (const std::exception& e) {
        mirage::log::error("fatal: {}", e.what());
        if (owns_runtime_files) {
            clear_runtime_files();
        }
        return 1;
    }
    if (owns_runtime_files) {
        clear_runtime_files();
    }
    mirage::log::info("stopped");
    return 0;
}
}  // namespace
int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string subcmd = argv[1];
        if (subcmd == "stop") {
            return handle_stop();
        }
        if (subcmd == "status") {
            bool verbose = false;
            for (int i = 2; i < argc; ++i) {
                if (std::string(argv[i]) == "-v" || std::string(argv[i]) == "--verbose") {
                    verbose = true;
                }
            }
            return handle_status(verbose);
        }
        if (subcmd == "paths") {
            return handle_paths(argc, argv);
        }
        if (subcmd == "doctor") {
            return handle_doctor(argc, argv);
        }
        if (subcmd == "service") {
            return handle_service_command(argc, argv);
        }
    }

    auto args = argv_view(argc, argv, 1);
    for (auto arg : args) {
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            std::println("mirage {}", MIRAGE_VERSION);
            return 0;
        }
    }

    auto parsed = mirage::parse_runtime_cli_options(args, default_config_file_path());
    if (!parsed) {
        print_cli_error(parsed.error());
        return parsed.error().exit_code();
    }
    setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);

#ifdef _WIN32
    if (parsed->background_child_mode) {
        if (!daemonize_child()) {
            return 1;
        }
        setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);
        return run_receiver(*parsed, true, false);
    }
    if (parsed->daemon_mode) {
        return daemonize_parent(argc, argv) ? 0 : 1;
    }
#else
    if (parsed->daemon_mode) {
        pid_t child_pid = 0;
        if (!daemonize(child_pid)) {
            return 1;
        }
        setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);
    }
#endif

    bool owns_runtime_files = parsed->daemon_mode;
    if (!parsed->daemon_mode) {
        if (!claim_runtime_for_process(mirage::current_process_id())) {
            return 1;
        }
        owns_runtime_files = true;
    }

    return run_receiver(*parsed, owns_runtime_files, !parsed->daemon_mode);
}
