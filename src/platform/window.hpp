#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

namespace mirage::platform {

class window {
public:
    static std::unique_ptr<window> create(std::string_view title, uint32_t w, uint32_t h);

    virtual ~window() = default;

    [[nodiscard]] virtual std::vector<const char*> required_vulkan_extensions() const = 0;
    virtual VkSurfaceKHR create_vulkan_surface(VkInstance instance) = 0;
    [[nodiscard]] virtual std::pair<uint32_t, uint32_t> size_pixels() const = 0;

    virtual bool poll_events() = 0;
    [[nodiscard]] virtual bool resize_pending() = 0;
    virtual void clear_resize_flag() = 0;
    virtual void wait_events() = 0;

    window() = default;
    window(const window&) = delete;
    window& operator=(const window&) = delete;
};

}  // namespace mirage::platform
