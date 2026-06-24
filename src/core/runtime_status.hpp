#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "core/core.hpp"
#include "core/receiver_adapter.hpp"
#include "core/receiver_client.hpp"
#include "core/receiver_source.hpp"

namespace mirage {

[[nodiscard]] bool write_runtime_status_json(const std::filesystem::path& status_path, int pid,
                                             const config& cfg, const std::string& ip,
                                             const std::string& iface_name,
                                             const std::filesystem::path& identity_path,
                                             int64_t started,
                                             std::span<const receiver_adapter_status> adapters,
                                             std::span<const receiver_source_descriptor> sources,
                                             std::span<const receiver_client_status> clients);

class runtime_status_tracker final : public receiver_session_observer {
public:
    runtime_status_tracker(std::filesystem::path status_path, int pid, const config& cfg,
                           std::string ip, std::string iface_name,
                           std::filesystem::path identity_path, int64_t started,
                           receiver_adapter_registry& adapters,
                           std::span<const receiver_source_descriptor> sources);

    uint64_t client_connected(receiver_client_status client) override;
    void client_disconnected(uint64_t client_id) override;
    void client_state_updated(uint64_t client_id, receiver_client_state state) override;
    void client_stream_updated(uint64_t client_id, receiver_client_stream_status stream) override;
    void client_media_updated(uint64_t client_id, receiver_client_media_status media) override;

    [[nodiscard]] bool write();

private:
    [[nodiscard]] bool write_unlocked();
    [[nodiscard]] bool has_client_unlocked(protocol protocol_id) const;

    mutable std::mutex mutex_;
    std::filesystem::path status_path_;
    int pid_;
    const config& cfg_;
    std::string ip_;
    std::string iface_name_;
    std::filesystem::path identity_path_;
    int64_t started_ = 0;
    receiver_adapter_registry& adapters_;
    std::span<const receiver_source_descriptor> sources_;
    std::vector<receiver_client_status> clients_;
    uint64_t next_client_id_ = 1;
};

}  // namespace mirage
