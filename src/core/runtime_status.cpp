#include "core/runtime_status.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/log.hpp"
#include "core/status_report.hpp"

namespace mirage {

bool write_runtime_status_json(const std::filesystem::path& status_path, int pid, const config& cfg,
                               const std::string& ip, const std::string& iface_name,
                               const std::filesystem::path& identity_path, int64_t started,
                               std::span<const receiver_adapter_status> adapters,
                               std::span<const receiver_source_descriptor> sources,
                               std::span<const receiver_client_status> clients) {
    auto tmp_path = status_path;
    tmp_path += ".tmp";
    auto identity_key = identity_path.string();
    std::error_code ec;
    std::filesystem::create_directories(status_path.parent_path(), ec);
    if (ec) {
        log::warn("could not create status directory {}: {}", status_path.parent_path().string(),
                  ec.message());
        return false;
    }
    std::ofstream file(tmp_path, std::ios::trunc);
    if (!file) {
        log::warn("could not write status file: {}", tmp_path.string());
        return false;
    }
    file << render_status_json({
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
    file.close();
    if (!file) {
        log::warn("could not finish writing status file: {}", tmp_path.string());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::filesystem::rename(tmp_path, status_path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(status_path, remove_ec);
        ec.clear();
        std::filesystem::rename(tmp_path, status_path, ec);
    }
    if (ec) {
        log::warn("could not publish status file {}: {}", status_path.string(), ec.message());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

runtime_status_tracker::runtime_status_tracker(std::filesystem::path status_path, int pid,
                                               const config& cfg, std::string ip,
                                               std::string iface_name,
                                               std::filesystem::path identity_path, int64_t started,
                                               receiver_adapter_registry& adapters,
                                               std::span<const receiver_source_descriptor> sources)
    : status_path_(std::move(status_path)),
      pid_(pid),
      cfg_(cfg),
      ip_(std::move(ip)),
      iface_name_(std::move(iface_name)),
      identity_path_(std::move(identity_path)),
      started_(started),
      adapters_(adapters),
      sources_(sources) {}

uint64_t runtime_status_tracker::client_connected(receiver_client_status client) {
    std::scoped_lock lock(mutex_);

    client.id = next_client_id_++;
    if (client.name.empty()) {
        client.name = std::string(protocol_id(client.protocol_id));
    }
    if (client.connected_at == 0) {
        client.connected_at =
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    }

    clients_.push_back(std::move(client));
    adapters_.mark_running(clients_.back().protocol_id);
    static_cast<void>(write_unlocked());
    return clients_.back().id;
}

void runtime_status_tracker::client_disconnected(uint64_t client_id) {
    std::scoped_lock lock(mutex_);

    auto it = std::ranges::find(clients_, client_id, &receiver_client_status::id);
    if (it == clients_.end()) {
        return;
    }

    const auto protocol = it->protocol_id;
    clients_.erase(it);
    if (!has_client_unlocked(protocol)) {
        if (auto* adapter = adapters_.find(protocol);
            adapter != nullptr && adapter->state == receiver_adapter_state::running) {
            adapters_.mark_listening(protocol);
        }
    }
    static_cast<void>(write_unlocked());
}

void runtime_status_tracker::client_stream_updated(uint64_t client_id,
                                                   receiver_client_stream_status stream) {
    std::scoped_lock lock(mutex_);

    auto client = std::ranges::find(clients_, client_id, &receiver_client_status::id);
    if (client == clients_.end()) {
        return;
    }

    auto existing =
        std::ranges::find(client->streams, stream.kind, &receiver_client_stream_status::kind);
    if (existing == client->streams.end()) {
        client->streams.push_back(std::move(stream));
    } else {
        *existing = std::move(stream);
    }
    static_cast<void>(write_unlocked());
}

void runtime_status_tracker::client_media_updated(uint64_t client_id,
                                                  receiver_client_media_status media) {
    std::scoped_lock lock(mutex_);

    auto client = std::ranges::find(clients_, client_id, &receiver_client_status::id);
    if (client == clients_.end()) {
        return;
    }

    client->media = std::move(media);
    static_cast<void>(write_unlocked());
}

bool runtime_status_tracker::write() {
    std::scoped_lock lock(mutex_);
    return write_unlocked();
}

bool runtime_status_tracker::write_unlocked() {
    return write_runtime_status_json(status_path_, pid_, cfg_, ip_, iface_name_, identity_path_,
                                     started_, adapters_.all(), sources_, clients_);
}

bool runtime_status_tracker::has_client_unlocked(protocol protocol_id) const {
    return std::ranges::any_of(
        clients_, [&](const auto& client) { return client.protocol_id == protocol_id; });
}

}  // namespace mirage
