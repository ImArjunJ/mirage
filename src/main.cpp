#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
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
    std::println(stderr, "mirage - airplay / cast / miracast receiver");
    std::println(stderr, "");
    std::println(stderr, "usage:");
    std::println(stderr, "  mirage                      run in foreground");
    std::println(stderr, "  mirage --daemon             run in background");
    std::println(stderr, "  mirage stop                 stop background instance");
    std::println(stderr, "  mirage status               show current state");
    std::println(stderr, "  mirage status -v            show detailed state");
    std::println(stderr, "  mirage paths                show runtime file paths");
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

std::filesystem::path identity_key_path(const mirage::config& cfg) {
    if (!cfg.identity_key_path.empty()) {
        return std::filesystem::path(cfg.identity_key_path);
    }
    return state_dir() / "identity.key";
}

std::string inspect_port_command(uint16_t port) {
#ifdef _WIN32
    return std::format("netstat -ano | findstr :{}", port);
#else
    return std::format("lsof -i :{}", port);
#endif
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

int handle_paths(int argc, char* argv[]) {
    mirage::config cfg;
    auto config_path = default_config_file_path();
    bool explicit_config = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
            explicit_config = true;
        } else if (arg == "--help" || arg == "-h") {
            std::println("usage: mirage paths [--config <file>]");
            return 0;
        } else {
            std::println(stderr, "unknown paths option: {}", arg);
            return 2;
        }
    }

    const bool config_exists = std::filesystem::exists(config_path);
    if (config_exists) {
        auto loaded = mirage::config::load_from_file(config_path.string());
        if (loaded) {
            cfg = *loaded;
        } else if (explicit_config) {
            std::println(stderr, "failed to load config: {}", loaded.error().message);
            return 1;
        }
    } else if (explicit_config) {
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

bool pid_is_running(int pid) {
#ifdef _WIN32
    return is_process_running(pid);
#else
    return is_process_running(static_cast<pid_t>(pid));
#endif
}

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
        if (object_start == std::string::npos) {
            return {};
        }
        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t pos = object_start; pos < json.size(); ++pos) {
            char c = json[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    return std::string_view(json).substr(object_start, pos - object_start + 1);
                }
            }
        }
        return {};
    };

    auto extract_array = [&](const std::string& key) -> std::string_view {
        auto needle = "\"" + key + "\":[";
        auto pos = json.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
        auto array_start = pos + needle.size() - 1;
        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t scan = array_start; scan < json.size(); ++scan) {
            char c = json[scan];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    return std::string_view(json).substr(array_start, scan - array_start + 1);
                }
            }
        }
        return {};
    };

    auto extract_array_from = [](std::string_view object,
                                 const std::string& key) -> std::string_view {
        auto needle = "\"" + key + "\":[";
        auto pos = object.find(needle);
        if (pos == std::string_view::npos) {
            return {};
        }
        auto array_start = pos + needle.size() - 1;
        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t scan = array_start; scan < object.size(); ++scan) {
            char c = object[scan];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '[') {
                ++depth;
            } else if (c == ']') {
                --depth;
                if (depth == 0) {
                    return object.substr(array_start, scan - array_start + 1);
                }
            }
        }
        return {};
    };

    auto extract_objects = [](std::string_view array) -> std::vector<std::string_view> {
        std::vector<std::string_view> objects;
        size_t object_start = std::string_view::npos;
        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t pos = 0; pos < array.size(); ++pos) {
            char c = array[pos];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                if (depth == 0) {
                    object_start = pos;
                }
                ++depth;
            } else if (c == '}' && depth > 0) {
                --depth;
                if (depth == 0 && object_start != std::string_view::npos) {
                    objects.push_back(array.substr(object_start, pos - object_start + 1));
                    object_start = std::string_view::npos;
                }
            }
        }
        return objects;
    };

    auto capability_summary = [&](std::string_view object) -> std::string {
        std::string summary;
        auto append = [&](const char* key, std::string_view label) {
            auto enabled = extract_bool_from(object, key);
            if (!enabled || !*enabled) {
                return;
            }
            if (!summary.empty()) {
                summary += "/";
            }
            summary += label;
        };
        append("audio", "audio");
        append("video", "video");
        append("app_lifecycle", "apps");
        append("media_control", "media");
        append("remote_control", "remote");
        append("metadata", "metadata");
        return summary;
    };

    auto is_default_protocol_detail = [](std::string_view detail) {
        return detail == "disabled by config" || detail == "rtsp/raop receiver" ||
               detail == "cast v2 control/status receiver" || detail == "wfd capability listener";
    };

    auto name = extract_string("name");
    auto ip = extract_string("ip");
    auto iface = extract_string("interface");
    auto client_objects = extract_objects(extract_array("clients"));

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
    if (!verbose && !client_objects.empty()) {
        std::println(stderr, "  clients: {}", client_objects.size());
    }

    if (verbose) {
        auto airplay_port = extract_int("airplay_port");
        auto cast_port = extract_int("cast_port");
        auto miracast_port = extract_int("miracast_port");
        if (airplay_port) {
            std::println(stderr, "  airplay port: {}", *airplay_port);
        }
        if (cast_port) {
            std::println(stderr, "  cast port: {}", *cast_port);
        }
        if (miracast_port) {
            std::println(stderr, "  miracast port: {}", *miracast_port);
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
            auto transport = extract_string_from(object, "transport");
            auto caps = capability_summary(object);
            const bool disabled = state == "disabled";
            std::string line = std::format("    {}: {}", id, state.empty() ? "unknown" : state);
            if (port && *port > 0) {
                line += std::format(", port {}", *port);
            }
            if (!disabled && !transport.empty()) {
                line += std::format(", transport {}", transport);
            }
            if (!disabled && !caps.empty()) {
                line += std::format(", {}", caps);
            }
            if (!disabled && advertised && *advertised) {
                line += ", advertised";
            }
            if (!detail.empty() && !is_default_protocol_detail(detail)) {
                line += std::format(", {}", detail);
            }
            std::println(stderr, "{}", line);
        }
        std::println(stderr, "  clients:");
        if (client_objects.empty()) {
            std::println(stderr, "    none");
        }
        for (auto object : client_objects) {
            auto protocol = extract_string_from(object, "protocol");
            auto address = extract_string_from(object, "address");
            auto state = extract_string_from(object, "state");
            auto client_name = extract_string_from(object, "name");

            std::string line = std::format("    {}", protocol.empty() ? "client" : protocol);
            if (!address.empty()) {
                line += std::format(": {}", address);
            }
            if (!state.empty()) {
                line += std::format(", {}", state);
            }
            if (!client_name.empty() && client_name != protocol) {
                line += std::format(", {}", client_name);
            }
            std::println(stderr, "{}", line);

            for (auto stream : extract_objects(extract_array_from(object, "streams"))) {
                auto kind = extract_string_from(stream, "kind");
                auto health = extract_string_from(stream, "health");
                auto reason = extract_string_from(stream, "reason");
                std::string stream_line =
                    std::format("      {}: {}", kind.empty() ? "stream" : kind,
                                health.empty() ? "unknown" : health);

                if (kind == "audio") {
                    auto decoded = extract_int_from(stream, "decoded_packets");
                    auto received = extract_int_from(stream, "received_packets");
                    if (decoded) {
                        stream_line += std::format(", decoded {}", *decoded);
                    }
                    if (received) {
                        stream_line += std::format(", received {}", *received);
                    }
                } else if (kind == "video") {
                    auto frames = extract_int_from(stream, "frames");
                    auto keyframes = extract_int_from(stream, "keyframes");
                    if (frames) {
                        stream_line += std::format(", frames {}", *frames);
                    }
                    if (keyframes) {
                        stream_line += std::format(", keyframes {}", *keyframes);
                    }
                }

                if (!reason.empty() && reason != "ok") {
                    stream_line += std::format(", reason {}", reason);
                }
                std::println(stderr, "{}", stream_line);
            }
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

bool write_pid_file(int pid) {
    auto dir = state_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::println(stderr, "could not create state directory: {}", ec.message());
        return false;
    }
    auto path = pid_file_path();
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        std::println(stderr, "could not write pid file: {}", path.string());
        return false;
    }
    f << pid;
    return true;
}

void remove_pid_file() {
    std::error_code ec;
    std::filesystem::remove(pid_file_path(), ec);
    std::filesystem::remove(status_file_path(), ec);
}

bool write_status_json(int pid, const mirage::config& cfg, const std::string& ip,
                       const std::string& iface_name, const std::filesystem::path& identity_path,
                       int64_t started,
                       std::span<const mirage::receiver_adapter_status> adapters,
                       std::span<const mirage::receiver_source_descriptor> sources,
                       std::span<const mirage::receiver_client_status> clients) {
    auto path = status_file_path();
    auto tmp_path = path;
    tmp_path += ".tmp";
    auto identity_key = identity_path.string();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        mirage::log::warn("could not create status directory {}: {}", path.parent_path().string(),
                          ec.message());
        return false;
    }
    std::ofstream f(tmp_path, std::ios::trunc);
    if (!f) {
        mirage::log::warn("could not write status file: {}", tmp_path.string());
        return false;
    }
    f << mirage::render_status_json({
        .pid = pid,
        .name = cfg.device_name,
        .ip = ip,
        .interface_name = iface_name,
        .identity_key = identity_key,
        .airplay_port = cfg.airplay_port,
        .cast_port = cfg.cast_port,
        .miracast_port = cfg.miracast_port,
        .started = started,
        .adapters = adapters,
        .sources = sources,
        .clients = clients,
    });
    f.close();
    if (!f) {
        mirage::log::warn("could not finish writing status file: {}", tmp_path.string());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
        ec.clear();
        std::filesystem::rename(tmp_path, path, ec);
    }
    if (ec) {
        mirage::log::warn("could not publish status file {}: {}", path.string(), ec.message());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

class runtime_status_tracker final : public mirage::receiver_session_observer {
public:
    runtime_status_tracker(int pid, const mirage::config& cfg, std::string ip,
                           std::string iface_name, std::filesystem::path identity_path,
                           int64_t started,
                           mirage::receiver_adapter_registry& adapters,
                           std::span<const mirage::receiver_source_descriptor> sources)
        : pid_(pid),
          cfg_(cfg),
          ip_(std::move(ip)),
          iface_name_(std::move(iface_name)),
          identity_path_(std::move(identity_path)),
          started_(started),
          adapters_(adapters),
          sources_(sources) {}

    uint64_t client_connected(mirage::receiver_client_status client) override {
        client.id = next_client_id_++;
        if (client.name.empty()) {
            client.name = std::string(mirage::protocol_id(client.protocol_id));
        }
        if (client.connected_at == 0) {
            client.connected_at = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
        }

        clients_.push_back(std::move(client));
        adapters_.mark_running(clients_.back().protocol_id);
        write();
        return clients_.back().id;
    }

    void client_disconnected(uint64_t client_id) override {
        auto it = std::ranges::find(clients_, client_id, &mirage::receiver_client_status::id);
        if (it == clients_.end()) {
            return;
        }

        const auto protocol_id = it->protocol_id;
        clients_.erase(it);
        if (!has_client(protocol_id)) {
            if (auto* adapter = adapters_.find(protocol_id);
                adapter != nullptr && adapter->state == mirage::receiver_adapter_state::running) {
                adapters_.mark_listening(protocol_id);
            }
        }
        write();
    }

    void client_stream_updated(uint64_t client_id,
                               mirage::receiver_client_stream_status stream) override {
        auto client = std::ranges::find(clients_, client_id, &mirage::receiver_client_status::id);
        if (client == clients_.end()) {
            return;
        }

        auto existing = std::ranges::find(client->streams, stream.kind,
                                          &mirage::receiver_client_stream_status::kind);
        if (existing == client->streams.end()) {
            client->streams.push_back(std::move(stream));
        } else {
            *existing = std::move(stream);
        }
        write();
    }

    bool write() {
        return write_status_json(pid_, cfg_, ip_, iface_name_, identity_path_, started_,
                                 adapters_.all(), sources_, clients_);
    }

private:
    bool has_client(mirage::protocol protocol_id) const {
        return std::ranges::any_of(clients_, [&](const auto& client) {
            return client.protocol_id == protocol_id;
        });
    }

    int pid_;
    const mirage::config& cfg_;
    std::string ip_;
    std::string iface_name_;
    std::filesystem::path identity_path_;
    int64_t started_ = 0;
    mirage::receiver_adapter_registry& adapters_;
    std::span<const mirage::receiver_source_descriptor> sources_;
    std::vector<mirage::receiver_client_status> clients_;
    uint64_t next_client_id_ = 1;
};

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
    if (!write_pid_file(current_pid())) {
        return false;
    }

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
        if (!write_pid_file(static_cast<int>(child_pid))) {
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

    if (!write_pid_file(current_pid())) {
        return false;
    }
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
        if (subcmd == "paths") {
            return handle_paths(argc, argv);
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
    bool owns_runtime_files = daemon_mode;
    if (!daemon_mode) {
        auto pid = read_pid_file();
        if (pid && pid_is_running(*pid)) {
            std::println(stderr, "mirage is already running (pid {}).", *pid);
            return 1;
        }
        remove_pid_file();
        if (!write_pid_file(current_pid())) {
            return 1;
        }
        owns_runtime_files = true;
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
            if (owns_runtime_files) {
                remove_pid_file();
            }
            return 1;
        }
        auto receiver_public_key = keypair->public_key();
        auto interfaces = mirage::discovery::enumerate_interfaces();
        if (!interfaces) {
            std::println(stderr, "no network interfaces found.");
            std::println(stderr, "  make sure wifi or ethernet is connected and up.");
            if (owns_runtime_files) {
                remove_pid_file();
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
        if (!no_mdns) {
            mdns.emplace(ctx);
            mdns_publisher.emplace(*mdns);
            discovery = &*mdns_publisher;
            mirage::log::info("built-in mdns broadcaster enabled");
        }
        std::vector<std::unique_ptr<mirage::receiver_session>> receiver_sessions;
        const auto started_at =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        runtime_status_tracker status_tracker(current_pid(), cfg, local_ip, iface_name,
                                              receiver_identity_path, started_at, adapters,
                                              receiver_sources.all());
        const mirage::receiver_source_runtime receiver_runtime{
            .io_context = &ctx,
            .receiver_identity = &*keypair,
            .receiver_public_key = &receiver_public_key,
            .session_observer = &status_tracker,
            .device_name = cfg.device_name,
            .mac_address = mac_address,
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
                print_receiver_start_error(source.id, source.port, session.error());
                continue;
            }
            start_receiver_session(std::move(*session));
        }
        if (mdns) {
            mirage::io::co_spawn(ctx, mdns->run());
        }

        status_tracker.write();

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
        if (owns_runtime_files) {
            remove_pid_file();
        }
        return 1;
    }
    if (owns_runtime_files) {
        remove_pid_file();
    }
    mirage::log::info("stopped");
    return 0;
}
