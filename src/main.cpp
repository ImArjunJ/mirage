#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#include <winsock2.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/core.hpp"
#include "core/log.hpp"
#include "core/receiver_adapter.hpp"
#include "core/receiver_session.hpp"
#include "crypto/crypto.hpp"
#include "discovery/discovery.hpp"
#include "io/event_loop.hpp"
#include "protocols/receiver_sessions.hpp"
namespace {
volatile std::sig_atomic_t signal_received = 0;
void signal_handler(int signal) {
    signal_received = signal;
}

void print_help() {
    std::println(stderr, "mirage - airplay / cast / miracast receiver");
    std::println(stderr, "");
    std::println(stderr, "usage:");
    std::println(stderr, "  mirage                      run in foreground");
    std::println(stderr, "  mirage --daemon             run in background");
    std::println(stderr, "  mirage stop                 stop background instance");
    std::println(stderr, "  mirage status               show current state");
    std::println(stderr, "  mirage status -v            show detailed state");
    std::println(stderr, "");
    std::println(stderr, "options:");
    std::println(stderr, "  --name <name>       device name (default: Mirage)");
    std::println(stderr, "  --port <port>       airplay port (default: 7000)");
    std::println(stderr, "  --cast-port <port>  cast port (default: 8009)");
    std::println(stderr, "  --miracast-port <port>");
    std::println(stderr, "                      miracast port (default: 7236)");
    std::println(stderr, "  --no-airplay        disable airplay");
    std::println(stderr, "  --cast              enable experimental google cast stub");
    std::println(stderr, "  --miracast          enable experimental miracast stub");
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

std::filesystem::path state_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("LOCALAPPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "mirage";
    }
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::filesystem::path(userprofile) / ".mirage";
    }
    return std::filesystem::path("C:\\ProgramData") / "mirage";
#else
    const char* xdg = std::getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "mirage";
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return std::filesystem::path(home) / ".local" / "state" / "mirage";
#endif
}

std::filesystem::path config_dir() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "mirage";
    }
    return state_dir();
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "mirage";
    }
    const char* home = std::getenv("HOME");
    if (!home) {
        home = "/tmp";
    }
    return std::filesystem::path(home) / ".config" / "mirage";
#endif
}

std::filesystem::path default_config_file_path() {
    return config_dir() / "config.conf";
}

std::filesystem::path pid_file_path() {
    return state_dir() / "mirage.pid";
}

std::filesystem::path status_file_path() {
    return state_dir() / "status.json";
}

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

std::string json_bool(bool value) {
    return value ? "true" : "false";
}

std::filesystem::path identity_key_path(const mirage::config& cfg) {
    if (!cfg.identity_key_path.empty()) {
        return std::filesystem::path(cfg.identity_key_path);
    }
    return state_dir() / "identity.key";
}

std::string trim_ascii(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                              value.back() == '\n')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t' ||
                                    value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }
    if (start > 0) {
        value.erase(0, start);
    }
    return value;
}

bool write_identity_key(const std::filesystem::path& path,
                        const std::array<std::byte, 32>& private_key) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            mirage::log::warn("could not create identity key directory {}: {}",
                              path.parent_path().string(), ec.message());
            return false;
        }
    }

    auto encoded = mirage::base64_encode(private_key);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        mirage::log::warn("could not write identity key: {}", path.string());
        return false;
    }
    file << encoded << "\n";
    file.close();
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    return true;
}

mirage::result<mirage::crypto::ed25519_keypair> load_or_create_identity_keypair(
    const mirage::config& cfg) {
    auto path = identity_key_path(cfg);
    if (std::filesystem::exists(path)) {
        std::ifstream file(path, std::ios::binary);
        if (file) {
            std::string encoded((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
            auto decoded = mirage::base64_decode(trim_ascii(std::move(encoded)));
            if (decoded && decoded->size() == 32) {
                std::array<std::byte, 32> private_key{};
                std::copy_n(decoded->begin(), private_key.size(), private_key.begin());
                auto keypair = mirage::crypto::ed25519_keypair::from_private_key(private_key);
                if (keypair) {
                    mirage::log::info("loaded persistent receiver identity: {}", path.string());
                    return keypair;
                }
                mirage::log::warn("identity key could not be loaded: {}", keypair.error().message);
            } else if (decoded) {
                mirage::log::warn("identity key has {} bytes, expected 32: {}", decoded->size(),
                                  path.string());
            } else {
                mirage::log::warn("identity key is not valid base64: {}", decoded.error().message);
            }
        } else {
            mirage::log::warn("could not open identity key: {}", path.string());
        }
    }

    auto keypair = mirage::crypto::ed25519_keypair::generate();
    if (!keypair) {
        return std::unexpected(keypair.error());
    }
    auto private_key = keypair->private_key();
    if (private_key) {
        if (write_identity_key(path, *private_key)) {
            mirage::log::info("created persistent receiver identity: {}", path.string());
        } else {
            mirage::log::warn("using transient receiver identity for this run");
        }
    } else {
        mirage::log::warn("could not persist receiver identity: {}", private_key.error().message);
    }
    return keypair;
}

std::optional<int> read_pid_file() {
    std::ifstream f(pid_file_path());
    if (!f.is_open()) {
        return std::nullopt;
    }
    int pid = 0;
    f >> pid;
    if (pid <= 0) {
        return std::nullopt;
    }
    return pid;
}

#ifdef _WIN32
bool is_process_running(int pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) {
        return false;
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(h, &exit_code);
    CloseHandle(h);
    return exit_code == STILL_ACTIVE;
}
#else
bool is_process_running(pid_t pid) {
    return kill(pid, 0) == 0;
}
#endif

#ifdef _WIN32
int handle_stop() {
    auto pid = read_pid_file();
    if (!pid || !is_process_running(*pid)) {
        std::println(stderr, "mirage is not running.");
        return 1;
    }
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(*pid));
    if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
    }
    for (int i = 0; i < 30; ++i) {
        Sleep(100);
        if (!is_process_running(*pid)) {
            std::println(stderr, "mirage stopped.");
            return 0;
        }
    }
    std::println(stderr, "mirage did not stop within 3 seconds (pid {}).", *pid);
    return 1;
}
#else
int handle_stop() {
    auto pid = read_pid_file();
    if (!pid || !is_process_running(static_cast<pid_t>(*pid))) {
        std::println(stderr, "mirage is not running.");
        return 1;
    }
    kill(static_cast<pid_t>(*pid), SIGTERM);
    for (int i = 0; i < 30; ++i) {
        usleep(100'000);
        if (!is_process_running(static_cast<pid_t>(*pid))) {
            std::println(stderr, "mirage stopped.");
            return 0;
        }
    }
    std::println(stderr, "mirage did not stop within 3 seconds (pid {}).", *pid);
    return 1;
}
#endif

int handle_status(bool verbose) {
    auto pid = read_pid_file();
    if (!pid || !is_process_running(
#ifdef _WIN32
                    *pid
#else
                    static_cast<pid_t>(*pid)
#endif
                    )) {
        std::println(stderr, "mirage is not running.");
        return 0;
    }
    auto path = status_file_path();
    std::ifstream f(path);
    if (!f.is_open()) {
        std::println(stderr, "mirage is running (pid {})", *pid);
        return 0;
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto extract_string = [&](const std::string& key) -> std::string {
        auto needle = "\"" + key + "\":\"";
        auto pos = json.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
        pos += needle.size();
        auto end = json.find('"', pos);
        if (end == std::string::npos) {
            return {};
        }
        return json.substr(pos, end - pos);
    };

    auto extract_int = [&](const std::string& key) -> std::optional<int64_t> {
        auto needle = "\"" + key + "\":";
        auto pos = json.find(needle);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        pos += needle.size();
        while (pos < json.size() && json[pos] == ' ') {
            ++pos;
        }
        try {
            return std::stoll(json.substr(pos));
        } catch (...) {
            return std::nullopt;
        }
    };

    auto extract_bool_from = [](std::string_view object,
                                const std::string& key) -> std::optional<bool> {
        auto needle = "\"" + key + "\":";
        auto pos = object.find(needle);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        pos += needle.size();
        while (pos < object.size() && object[pos] == ' ') {
            ++pos;
        }
        if (object.substr(pos, 4) == "true") {
            return true;
        }
        if (object.substr(pos, 5) == "false") {
            return false;
        }
        return std::nullopt;
    };

    auto extract_string_from = [](std::string_view object, const std::string& key) -> std::string {
        auto needle = "\"" + key + "\":\"";
        auto pos = object.find(needle);
        if (pos == std::string_view::npos) {
            return {};
        }
        pos += needle.size();
        auto end = object.find('"', pos);
        if (end == std::string_view::npos) {
            return {};
        }
        return std::string(object.substr(pos, end - pos));
    };

    auto extract_int_from = [](std::string_view object,
                               const std::string& key) -> std::optional<int64_t> {
        auto needle = "\"" + key + "\":";
        auto pos = object.find(needle);
        if (pos == std::string_view::npos) {
            return std::nullopt;
        }
        pos += needle.size();
        while (pos < object.size() && object[pos] == ' ') {
            ++pos;
        }
        try {
            return std::stoll(std::string(object.substr(pos)));
        } catch (...) {
            return std::nullopt;
        }
    };

    auto extract_protocol_object = [&](std::string_view id) -> std::string_view {
        auto needle = std::string{"\"id\":\""} + std::string{id} + "\"";
        auto id_pos = json.find(needle);
        if (id_pos == std::string::npos) {
            return {};
        }
        auto object_start = json.rfind('{', id_pos);
        auto object_end = json.find('}', id_pos);
        if (object_start == std::string::npos || object_end == std::string::npos ||
            object_end <= object_start) {
            return {};
        }
        return std::string_view(json).substr(object_start, object_end - object_start + 1);
    };

    auto name = extract_string("name");
    auto ip = extract_string("ip");
    auto iface = extract_string("interface");

    std::println(stderr, "mirage is running (pid {})", *pid);
    if (!name.empty()) {
        std::println(stderr, "  name: {}", name);
    }
    if (!ip.empty()) {
        if (!iface.empty()) {
            std::println(stderr, "  ip: {} ({})", ip, iface);
        } else {
            std::println(stderr, "  ip: {}", ip);
        }
    }

    if (verbose) {
        auto airplay_port = extract_int("airplay_port");
        auto cast_port = extract_int("cast_port");
        if (airplay_port) {
            std::println(stderr, "  airplay port: {}", *airplay_port);
        }
        if (cast_port) {
            std::println(stderr, "  cast port: {}", *cast_port);
        }
        auto identity_key = extract_string("identity_key");
        if (!identity_key.empty()) {
            std::println(stderr, "  identity key: {}", identity_key);
        }
        std::println(stderr, "  protocols:");
        for (auto id : {"airplay", "cast", "miracast"}) {
            auto object = extract_protocol_object(id);
            if (object.empty()) {
                continue;
            }
            auto state = extract_string_from(object, "state");
            auto port = extract_int_from(object, "port");
            auto advertised = extract_bool_from(object, "advertised");
            auto detail = extract_string_from(object, "detail");
            std::string line = std::format("    {}: {}", id, state.empty() ? "unknown" : state);
            if (port && *port > 0) {
                line += std::format(", port {}", *port);
            }
            if (advertised) {
                line += std::format(", advertised {}", *advertised ? "yes" : "no");
            }
            if (!detail.empty()) {
                line += std::format(", {}", detail);
            }
            std::println(stderr, "{}", line);
        }
        auto started = extract_int("started");
        if (started) {
            auto now = std::chrono::system_clock::now();
            auto started_time =
                std::chrono::system_clock::from_time_t(static_cast<time_t>(*started));
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_time);
            auto hours = uptime.count() / 3600;
            auto minutes = (uptime.count() % 3600) / 60;
            auto secs = uptime.count() % 60;
            if (hours > 0) {
                std::println(stderr, "  uptime: {}h {}m {}s", hours, minutes, secs);
            } else if (minutes > 0) {
                std::println(stderr, "  uptime: {}m {}s", minutes, secs);
            } else {
                std::println(stderr, "  uptime: {}s", secs);
            }
        }
    }
    return 0;
}

void write_pid_file(int pid) {
    auto dir = state_dir();
    std::filesystem::create_directories(dir);
    std::ofstream f(pid_file_path());
    f << pid;
}

void remove_pid_file() {
    std::error_code ec;
    std::filesystem::remove(pid_file_path(), ec);
    std::filesystem::remove(status_file_path(), ec);
}

void write_status_json(int pid, const mirage::config& cfg, const std::string& ip,
                       const std::string& iface_name, const std::filesystem::path& identity_path,
                       std::span<const mirage::receiver_adapter_status> adapters) {
    auto now = std::chrono::system_clock::now();
    auto started = std::chrono::system_clock::to_time_t(now);
    auto path = status_file_path();
    std::ofstream f(path);
    f << "{";
    f << "\"pid\":" << pid;
    f << ",\"name\":\"" << json_escape(cfg.device_name) << "\"";
    f << ",\"ip\":\"" << json_escape(ip) << "\"";
    f << ",\"interface\":\"" << json_escape(iface_name) << "\"";
    f << ",\"identity_key\":\"" << json_escape(identity_path.string()) << "\"";
    f << ",\"airplay_port\":" << cfg.airplay_port;
    f << ",\"cast_port\":" << cfg.cast_port;
    f << ",\"started\":" << started;
    f << ",\"protocols\":[";
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& adapter = adapters[i];
        if (i > 0) {
            f << ",";
        }
        f << "{";
        f << "\"id\":\"" << mirage::protocol_id(adapter.id) << "\"";
        f << ",\"name\":\"" << mirage::to_string(adapter.id) << "\"";
        f << ",\"state\":\"" << mirage::to_string(adapter.state) << "\"";
        f << ",\"port\":" << adapter.port;
        f << ",\"advertised\":" << json_bool(adapter.advertised);
        f << ",\"experimental\":" << json_bool(adapter.experimental);
        f << ",\"detail\":\"" << json_escape(adapter.detail) << "\"";
        f << "}";
    }
    f << "]";
    f << ",\"clients\":[]";
    f << "}";
}

void print_receiver_start_error(mirage::protocol id, uint16_t port,
                                const mirage::mirage_error& error) {
    switch (id) {
        case mirage::protocol::airplay:
            std::println(stderr, "could not start airplay on port {}.", port);
            std::println(stderr,
                         "  the port may be in use. try --port <port> to use a different one,");
            std::println(stderr, "  or run: lsof -i :{}", port);
            break;
        case mirage::protocol::cast:
            std::println(stderr, "could not start cast on port {}.", port);
            std::println(stderr, "  the port may be in use. try a different port or check:");
            std::println(stderr, "  lsof -i :{}", port);
            break;
        case mirage::protocol::miracast:
            mirage::log::error("failed to start miracast: {}", error.message);
            break;
    }
}

int current_pid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

#ifdef _WIN32
bool daemonize() {
    auto pid = read_pid_file();
    if (pid && is_process_running(*pid)) {
        std::println(stderr, "mirage is already running (pid {}).", *pid);
        return false;
    }
    auto dir = state_dir();
    std::filesystem::create_directories(dir);
    FreeConsole();
    write_pid_file(current_pid());

    auto log_path = dir / "mirage.log";
    FILE* log_file = nullptr;
    _wfreopen_s(&log_file, log_path.c_str(), L"a", stderr);

    std::println(stderr, "mirage started as background process (pid {})", current_pid());
    return true;
}
#else
bool daemonize(pid_t& child_pid) {
    auto pid = read_pid_file();
    if (pid && is_process_running(static_cast<pid_t>(*pid))) {
        std::println(stderr, "mirage is already running (pid {}).", *pid);
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
        write_pid_file(static_cast<int>(child_pid));
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

    write_pid_file(current_pid());
    return true;
}
#endif
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
    }

    mirage::config cfg;
    std::string config_file;
    bool explicit_config_file = false;
    bool debug = false;
    bool trace = false;
    bool diagnostics = false;
    bool verbose = false;
    bool no_mdns = false;
    bool daemon_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        }
        if (arg == "--version" || arg == "-V") {
            std::println("mirage {}", MIRAGE_VERSION);
            return 0;
        }
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
            explicit_config_file = true;
        } else if ((arg == "--name" || arg == "--port" || arg == "--identity-key") &&
                   i + 1 < argc) {
            ++i;
        }
    }
    if (config_file.empty()) {
        auto default_config = default_config_file_path();
        if (std::filesystem::exists(default_config)) {
            config_file = default_config.string();
        }
    }
    if (!config_file.empty()) {
        auto loaded = mirage::config::load_from_file(config_file);
        if (loaded) {
            cfg = *loaded;
        } else if (explicit_config_file) {
            mirage::log::warn("failed to load config: {}", loaded.error().message);
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            ++i;
        } else if (arg == "--name" && i + 1 < argc) {
            cfg.device_name = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.airplay_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--cast-port" && i + 1 < argc) {
            cfg.cast_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--miracast-port" && i + 1 < argc) {
            cfg.miracast_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--identity-key" && i + 1 < argc) {
            cfg.identity_key_path = argv[++i];
        } else if (arg == "--no-airplay") {
            cfg.enable_airplay = false;
        } else if (arg == "--cast") {
            cfg.enable_cast = true;
        } else if (arg == "--miracast") {
            cfg.enable_miracast = true;
        } else if (arg == "--no-cast") {
            cfg.enable_cast = false;
        } else if (arg == "--no-miracast") {
            cfg.enable_miracast = false;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--trace") {
            trace = true;
        } else if (arg == "--diagnostics") {
            diagnostics = true;
        } else if (arg == "--no-mdns") {
            no_mdns = true;
        } else if (arg == "--daemon" || arg == "-d") {
            daemon_mode = true;
        }
    }
    setup_logging(verbose, debug, trace, diagnostics);

    if (daemon_mode) {
#ifdef _WIN32
        if (!daemonize()) {
            return 1;
        }
#else
        pid_t child_pid = 0;
        if (!daemonize(child_pid)) {
            return 1;
        }
#endif
        setup_logging(verbose, debug, trace, diagnostics);
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    try {
        mirage::io::io_context ctx;
        auto receiver_identity_path = identity_key_path(cfg);
        auto keypair = load_or_create_identity_keypair(cfg);
        if (!keypair) {
            std::println(stderr, "crypto error: could not load or generate receiver identity.");
            std::println(stderr, "  {}", keypair.error().message);
            std::println(stderr, "  this is unusual -- check that openssl is installed correctly.");
            return 1;
        }
        auto interfaces = mirage::discovery::enumerate_interfaces();
        if (!interfaces) {
            std::println(stderr, "no network interfaces found.");
            std::println(stderr, "  make sure wifi or ethernet is connected and up.");
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
        if (!no_mdns) {
            mdns.emplace(ctx);
            mdns_publisher.emplace(*mdns);
            discovery = &*mdns_publisher;
            mirage::log::info("built-in mdns broadcaster enabled");
        }
        std::vector<std::unique_ptr<mirage::receiver_session>> receiver_sessions;
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
            auto session = mirage::protocols::make_receiver_session(ctx, source, &*keypair,
                                                                    cfg.device_name, mac_address);
            if (!session) {
                print_receiver_start_error(source.id, source.port, session.error());
                continue;
            }
            start_receiver_session(std::move(*session));
        }
        if (mdns) {
            mirage::io::co_spawn(ctx, mdns->run());
        }

        if (daemon_mode) {
            write_status_json(current_pid(), cfg, local_ip, iface_name, receiver_identity_path,
                              adapters.all());
        }

        mirage::log::user("mirage started{}",
                          local_ip.empty() ? "" : std::format(" on {}", local_ip));
        for (const auto& source : receiver_sources.all()) {
            if (!source.enabled) {
                continue;
            }
            if (source.port != 0) {
                mirage::log::user("  {} on port {}", mirage::protocol_id(source.id), source.port);
            } else {
                mirage::log::user("  {} enabled", mirage::protocol_id(source.id));
            }
        }
        if (!daemon_mode) {
            mirage::log::user("  press ctrl+c to stop.");
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
        if (daemon_mode) {
            remove_pid_file();
        }
        return 1;
    }
    if (daemon_mode) {
        remove_pid_file();
    }
    mirage::log::info("stopped");
    return 0;
}
