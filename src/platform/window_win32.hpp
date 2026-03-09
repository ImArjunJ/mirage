#pragma once
#ifdef _WIN32
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "window.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mirage::platform {

class win32_window : public window {
public:
    static std::unique_ptr<window> try_create(std::string_view title, uint32_t w, uint32_t h);
    ~win32_window() override;

    [[nodiscard]] std::vector<const char*> required_vulkan_extensions() const override;
    VkSurfaceKHR create_vulkan_surface(VkInstance instance) override;
    [[nodiscard]] std::pair<uint32_t, uint32_t> size_pixels() const override;
    bool poll_events() override;
    [[nodiscard]] bool resize_pending() override;
    void clear_resize_flag() override;
    void wait_events() override;

private:
    win32_window() = default;
    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    ATOM class_atom_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool closed_ = false;
    bool resized_ = false;
};

}  // namespace mirage::platform
#endif
