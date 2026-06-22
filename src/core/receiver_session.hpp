#pragma once

#include <cstdint>
#include <string_view>

#include "core/core.hpp"
#include "core/receiver_adapter.hpp"
#include "core/receiver_source.hpp"
#include "io/io.hpp"

namespace mirage {

namespace discovery {
class service_publisher;
}

using receiver_session_capabilities = receiver_source_capabilities;

class receiver_session {
public:
    virtual ~receiver_session() = default;

    [[nodiscard]] virtual protocol id() const = 0;
    [[nodiscard]] virtual uint16_t port() const = 0;
    [[nodiscard]] virtual receiver_session_capabilities capabilities() const = 0;

    virtual result<void> start(receiver_adapter_registry& adapters,
                               discovery::service_publisher& discovery) = 0;
    virtual io::task<void> run() = 0;
    virtual void stop(receiver_adapter_registry& adapters,
                      discovery::service_publisher& discovery) = 0;
};

}  // namespace mirage
