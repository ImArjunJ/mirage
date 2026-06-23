#include <algorithm>
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

#include "core/core.hpp"
#include "core/cli_options.hpp"
#include "core/log.hpp"
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
    return mirage::runtime_default_config_file_path(
        mirage::current_runtime_path_environment());
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

void log_receiver_identity_key_result(
    const std::filesystem::path& path, const mirage::receiver_identity_keypair& identity) {
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

    auto enabled_count = std::ranges::count_if(sources, [](const auto& source) {
        return source.enabled;
    });
    if (enabled_count == 0) {
        ok = false;
        std::println("check: no receiver protocols enabled");
    }

    for (size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].enabled || sources[i].port == 0) {
            continue;
        }
        for (size_t j = i + 1; j < sources.size(); ++j) {
            if (sources[j].enabled && sources[j].port == sources[i].port) {
                ok = false;
                std::println("check: {} and {} share port {}", mirage::protocol_id(sources[i].id),
                             mirage::protocol_id(sources[j].id), sources[i].port);
            }
        }
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

    std::println("dependencies: openssl linked, ffmpeg linked, vulkan linked");
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

#ifdef _WIN32
bool daemonize() {
    if (!clear_stale_runtime_before_daemon()) {
        return false;
    }
    auto dir = state_dir();
    std::filesystem::create_directories(dir);
    FreeConsole();
    if (!write_runtime_pid_or_report(mirage::current_process_id())) {
        return false;
    }

    auto log_path = dir / "mirage.log";
    FILE* log_file = nullptr;
    _wfreopen_s(&log_file, log_path.c_str(), L"a", stderr);

    std::println(stderr, "mirage started as background process (pid {})",
                 mirage::current_process_id());
    return true;
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
    const auto cfg = parsed->cfg;
    setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);

    if (parsed->daemon_mode) {
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
        setup_logging(parsed->verbose, parsed->debug, parsed->trace, parsed->diagnostics);
    }
    bool owns_runtime_files = parsed->daemon_mode;
    if (!parsed->daemon_mode) {
        if (!claim_runtime_for_process(mirage::current_process_id())) {
            return 1;
        }
        owns_runtime_files = true;
    }

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
        if (!parsed->no_mdns) {
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
        if (!parsed->daemon_mode) {
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
