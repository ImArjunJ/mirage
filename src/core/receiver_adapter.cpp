#include "core/receiver_adapter.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace mirage {
namespace {

receiver_adapter_status make_adapter(protocol id, bool enabled, uint16_t port,
                                     std::string_view enabled_detail) {
    return {
        .id = id,
        .state = enabled ? receiver_adapter_state::unavailable : receiver_adapter_state::disabled,
        .port = port,
        .advertised = false,
        .experimental = true,
        .detail = enabled ? std::string(enabled_detail) : "disabled by config",
    };
}

}  // namespace

receiver_adapter_registry::receiver_adapter_registry(const config& cfg)
    : adapters_{
          make_adapter(protocol::airplay, cfg.enable_airplay, cfg.airplay_port,
                       "rtsp/raop receiver"),
          make_adapter(protocol::cast, cfg.enable_cast, cfg.cast_port, "cast v2 stub"),
          make_adapter(protocol::miracast, cfg.enable_miracast, cfg.miracast_port, "wfd stub"),
      } {}

std::span<const receiver_adapter_status> receiver_adapter_registry::all() const {
    return {adapters_.data(), adapters_.size()};
}

receiver_adapter_status* receiver_adapter_registry::find(protocol id) {
    auto it = std::ranges::find(adapters_, id, &receiver_adapter_status::id);
    if (it == adapters_.end()) {
        return nullptr;
    }
    return &*it;
}

const receiver_adapter_status* receiver_adapter_registry::find(protocol id) const {
    auto it = std::ranges::find(adapters_, id, &receiver_adapter_status::id);
    if (it == adapters_.end()) {
        return nullptr;
    }
    return &*it;
}

void receiver_adapter_registry::mark_advertised(protocol id) {
    if (auto* adapter = find(id)) {
        adapter->advertised = true;
    }
}

void receiver_adapter_registry::set_detail(protocol id, std::string detail) {
    if (auto* adapter = find(id)) {
        adapter->detail = std::move(detail);
    }
}

void receiver_adapter_registry::mark_listening(protocol id) {
    set_state(id, receiver_adapter_state::listening);
}

void receiver_adapter_registry::mark_running(protocol id) {
    set_state(id, receiver_adapter_state::running);
}

void receiver_adapter_registry::mark_error(protocol id, std::string detail) {
    if (auto* adapter = find(id)) {
        adapter->state = receiver_adapter_state::error;
        adapter->detail = std::move(detail);
    }
}

void receiver_adapter_registry::mark_stopped(protocol id) {
    set_state(id, receiver_adapter_state::stopped);
}

void receiver_adapter_registry::set_state(protocol id, receiver_adapter_state state) {
    if (auto* adapter = find(id)) {
        adapter->state = state;
    }
}

}  // namespace mirage
