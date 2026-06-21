#pragma once

#include <span>
#include <string>
#include <vector>

#include "core/core.hpp"

namespace mirage {

class receiver_adapter_registry {
public:
    explicit receiver_adapter_registry(const config& cfg);

    [[nodiscard]] std::span<const receiver_adapter_status> all() const;
    [[nodiscard]] receiver_adapter_status* find(protocol id);
    [[nodiscard]] const receiver_adapter_status* find(protocol id) const;

    void mark_advertised(protocol id);
    void set_detail(protocol id, std::string detail);
    void mark_listening(protocol id);
    void mark_running(protocol id);
    void mark_error(protocol id, std::string detail);
    void mark_stopped(protocol id);

private:
    void set_state(protocol id, receiver_adapter_state state);

    std::vector<receiver_adapter_status> adapters_;
};

}  // namespace mirage
