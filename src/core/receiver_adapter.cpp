#include "core/receiver_adapter.hpp"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace mirage {
namespace {

receiver_adapter_status make_adapter(protocol id, bool enabled, uint16_t port, bool experimental,
                                     std::string_view enabled_detail) {
    return {
        .id = id,
        .state = enabled ? receiver_adapter_state::unavailable : receiver_adapter_state::disabled,
        .port = port,
        .advertised = false,
        .experimental = experimental,
        .detail = enabled ? std::string(enabled_detail) : "disabled by config",
        .default_detail = enabled ? std::string(enabled_detail) : "disabled by config",
    };
}

receiver_adapter_status make_adapter(const receiver_source_descriptor& source) {
    return make_adapter(source.id, source.enabled, source.port, source.experimental, source.detail);
}

}  // namespace

receiver_adapter_registry::receiver_adapter_registry(const config& cfg)
    : adapters_{
          make_adapter(protocol::airplay, cfg.enable_airplay, cfg.airplay_port, true,
                       "rtsp/raop receiver"),
          make_adapter(protocol::cast, cfg.enable_cast, cfg.cast_port, true,
                       "cast v2 probe receiver"),
          make_adapter(protocol::miracast, cfg.enable_miracast, cfg.miracast_port, true,
                       "wfd capability listener"),
      } {}

receiver_adapter_registry::receiver_adapter_registry(
    std::span<const receiver_source_descriptor> sources) {
    adapters_.reserve(sources.size());
    for (const auto& source : sources) {
        adapters_.push_back(make_adapter(source));
    }
}

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
        adapter->advertised = false;
        adapter->detail = std::move(detail);
    }
}

void receiver_adapter_registry::mark_stopped(protocol id) {
    set_state(id, receiver_adapter_state::stopped);
}

void receiver_adapter_registry::set_state(protocol id, receiver_adapter_state state) {
    if (auto* adapter = find(id)) {
        adapter->state = state;
        switch (state) {
            case receiver_adapter_state::disabled:
            case receiver_adapter_state::stopped:
                adapter->advertised = false;
                adapter->detail = adapter->default_detail;
                break;
            case receiver_adapter_state::listening:
            case receiver_adapter_state::running:
                adapter->detail = adapter->default_detail;
                break;
            case receiver_adapter_state::unavailable:
            case receiver_adapter_state::error:
                break;
        }
    }
}

}  // namespace mirage
