#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/core.hpp"
#include "core/log.hpp"
#include "crypto/crypto.hpp"
#include "discovery/discovery.hpp"
#include "protocols/protocols.hpp"
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
    std::println(stderr, "  --no-airplay        disable airplay");
    std::println(stderr, "  --no-cast           disable google cast");
    std::println(stderr, "  --no-miracast       disable miracast");
    std::println(stderr, "  --verbose           show more output");
    std::println(stderr, "  --debug             show everything");
    std::println(stderr, "  --config <file>     config file path");
    std::println(stderr, "  --version           print version");
    std::println(stderr, "  --help              show this help");
}

void setup_logging(bool verbose, bool debug) {
    if (debug) {
        mirage::log::min_level = mirage::log::level::debug;
    } else if (verbose) {
        mirage::log::min_level = mirage::log::level::info;
    } else {
        mirage::log::min_level = mirage::log::level::user;
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

std::filesystem::path pid_file_path() {
    return state_dir() / "mirage.pid";
}

std::filesystem::path status_file_path() {
    return state_dir() / "status.json";
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
                       const std::string& iface_name) {
    auto now = std::chrono::system_clock::now();
    auto started = std::chrono::system_clock::to_time_t(now);
    auto path = status_file_path();
    std::ofstream f(path);
    f << "{";
    f << "\"pid\":" << pid;
    f << ",\"name\":\"" << cfg.device_name << "\"";
    f << ",\"ip\":\"" << ip << "\"";
    f << ",\"interface\":\"" << iface_name << "\"";
    f << ",\"airplay_port\":" << cfg.airplay_port;
    f << ",\"cast_port\":" << cfg.cast_port;
    f << ",\"started\":" << started;
    f << ",\"clients\":[]";
    f << "}";
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
    bool debug = false;
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
        } else if (arg == "--name" && i + 1 < argc) {
            cfg.device_name = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            cfg.airplay_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--no-airplay") {
            cfg.enable_airplay = false;
        } else if (arg == "--no-cast") {
            cfg.enable_cast = false;
        } else if (arg == "--no-miracast") {
            cfg.enable_miracast = false;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--debug") {
            debug = true;
        } else if (arg == "--no-mdns") {
            no_mdns = true;
        } else if (arg == "--daemon" || arg == "-d") {
            daemon_mode = true;
        }
    }
    if (!config_file.empty() && std::filesystem::exists(config_file)) {
        auto loaded = mirage::config::load_from_file(config_file);
        if (loaded) {
            cfg = *loaded;
        } else {
            mirage::log::warn("failed to load config: {}", loaded.error().message);
        }
    }
    setup_logging(verbose, debug);

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
        setup_logging(verbose, debug);
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    try {
        mirage::io::io_context ctx;
        auto keypair = mirage::crypto::ed25519_keypair::generate();
        if (!keypair) {
            std::println(stderr, "crypto error: could not generate keypair.");
            std::println(stderr, "  this is unusual -- check that openssl is installed correctly.");
            return 1;
        }
        auto pubkey = keypair->public_key();
        mirage::log::info("generated ed25519 public key");
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
        std::optional<mirage::discovery::mdns_broadcaster> mdns;
        if (!no_mdns) {
            mdns.emplace(ctx);
            mirage::log::info("built-in mdns broadcaster enabled");
        }
        if (cfg.enable_airplay && mdns) {
            auto airplay_service = mirage::discovery::create_airplay_service(
                cfg.device_name, cfg.airplay_port, pubkey, mac_address);
            if (auto res = mdns->register_service(std::move(airplay_service)); !res) {
                mirage::log::error("failed to register airplay service: {}", res.error().message);
            }
            auto raop_service = mirage::discovery::create_raop_service(
                cfg.device_name, cfg.airplay_port, mac_address);
            if (auto res = mdns->register_service(std::move(raop_service)); !res) {
                mirage::log::error("failed to register raop service: {}", res.error().message);
            }
            mirage::log::info("airplay enabled on port {}", cfg.airplay_port);
        }
        if (cfg.enable_cast && mdns) {
            auto uuid = mirage::generate_uuid();
            auto cast_service =
                mirage::discovery::create_cast_service(cfg.device_name, cfg.cast_port, uuid);
            if (auto res = mdns->register_service(std::move(cast_service)); !res) {
                mirage::log::error("failed to register cast service: {}", res.error().message);
            }
            mirage::log::info("cast enabled on port {} (uuid: {})", cfg.cast_port, uuid);
        }
        if (cfg.enable_miracast) {
            mirage::log::info("miracast enabled (stub)");
        }
        std::optional<mirage::protocols::rtsp_server> rtsp;
        if (cfg.enable_airplay) {
            auto server =
                mirage::protocols::rtsp_server::bind(ctx, cfg.airplay_port, std::move(*keypair));
            if (server) {
                rtsp.emplace(std::move(*server));
                mirage::log::info("rtsp server on port {}", cfg.airplay_port);
            } else {
                std::println(stderr, "could not start airplay on port {}.", cfg.airplay_port);
                std::println(stderr,
                             "  the port may be in use. try --port <port> to use a different one,");
                std::println(stderr, "  or run: lsof -i :{}", cfg.airplay_port);
            }
        }
        std::optional<mirage::protocols::cast_receiver> cast;
        if (cfg.enable_cast) {
            auto receiver = mirage::protocols::cast_receiver::bind(ctx, cfg.cast_port);
            if (receiver) {
                cast.emplace(std::move(*receiver));
                mirage::log::info("cast receiver on port {}", cfg.cast_port);
            } else {
                std::println(stderr, "could not start cast on port {}.", cfg.cast_port);
                std::println(stderr, "  the port may be in use. try a different port or check:");
                std::println(stderr, "  lsof -i :{}", cfg.cast_port);
            }
        }
        if (mdns) {
            mirage::io::co_spawn(ctx, mdns->run());
        }
        if (rtsp) {
            mirage::io::co_spawn(ctx, rtsp->run());
        }
        if (cast) {
            mirage::io::co_spawn(ctx, cast->run());
        }

        if (daemon_mode) {
            write_status_json(current_pid(), cfg, local_ip, iface_name);
        }

        mirage::log::user("mirage started{}",
                          local_ip.empty() ? "" : std::format(" on {}", local_ip));
        if (cfg.enable_airplay) {
            mirage::log::user("  airplay on port {}", cfg.airplay_port);
        }
        if (cfg.enable_cast) {
            mirage::log::user("  cast on port {}", cfg.cast_port);
        }
        if (!daemon_mode) {
            mirage::log::user("  press ctrl+c to stop.");
        }
        while (signal_received == 0) {
            ctx.run_for(std::chrono::milliseconds(100));
        }
        mirage::log::info("received signal {}, shutting down", static_cast<int>(signal_received));
        if (mdns) {
            mdns->stop();
        }
        if (rtsp) {
            rtsp->stop();
        }
        if (cast) {
            cast->stop();
        }
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
