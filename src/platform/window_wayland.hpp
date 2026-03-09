#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

#include "dl.hpp"
#include "window.hpp"

struct wl_display;
struct wl_proxy;

struct wl_message {
    const char* name;
    const char* signature;
    const struct wl_interface** types;
};

struct wl_interface {
    const char* name;
    int version;
    int method_count;
    const struct wl_message* methods;
    int event_count;
    const struct wl_message* events;
};

namespace mirage::platform {

class wayland_window : public window {
public:
    static std::unique_ptr<window> try_create(std::string_view title, uint32_t w, uint32_t h);

    ~wayland_window() override;

    [[nodiscard]] std::vector<const char*> required_vulkan_extensions() const override;
    VkSurfaceKHR create_vulkan_surface(VkInstance instance) override;
    [[nodiscard]] std::pair<uint32_t, uint32_t> size_pixels() const override;

    bool poll_events() override;
    [[nodiscard]] bool resize_pending() override;
    void clear_resize_flag() override;
    void wait_events() override;

private:
    using display_connect_t = wl_display* (*)(const char*);
    using display_disconnect_t = void (*)(wl_display*);
    using display_dispatch_t = int (*)(wl_display*);
    using display_dispatch_pending_t = int (*)(wl_display*);
    using display_roundtrip_t = int (*)(wl_display*);
    using display_flush_t = int (*)(wl_display*);
    using display_get_fd_t = int (*)(wl_display*);
    using display_prepare_read_t = int (*)(wl_display*);
    using display_read_events_t = int (*)(wl_display*);
    using display_cancel_read_t = void (*)(wl_display*);
    using proxy_marshal_flags_t = wl_proxy* (*)(wl_proxy*, uint32_t, const wl_interface*, uint32_t,
                                                uint32_t, ...);
    using proxy_add_listener_t = int (*)(wl_proxy*, void (**)(void), void*);
    using proxy_destroy_t = void (*)(wl_proxy*);
    using proxy_get_version_t = uint32_t (*)(wl_proxy*);

    dl_lib lib_;
    wl_display* display_ = nullptr;
    wl_proxy* registry_ = nullptr;
    wl_proxy* compositor_ = nullptr;
    wl_proxy* surface_ = nullptr;
    wl_proxy* wm_base_ = nullptr;
    wl_proxy* xdg_surface_ = nullptr;
    wl_proxy* toplevel_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool closed_ = false;
    bool resized_ = false;
    bool configured_ = false;

    display_connect_t fn_connect_ = nullptr;
    display_disconnect_t fn_disconnect_ = nullptr;
    display_dispatch_t fn_dispatch_ = nullptr;
    display_dispatch_pending_t fn_dispatch_pending_ = nullptr;
    display_roundtrip_t fn_roundtrip_ = nullptr;
    display_flush_t fn_flush_ = nullptr;
    display_get_fd_t fn_get_fd_ = nullptr;
    display_prepare_read_t fn_prepare_read_ = nullptr;
    display_read_events_t fn_read_events_ = nullptr;
    display_cancel_read_t fn_cancel_read_ = nullptr;
    proxy_marshal_flags_t fn_marshal_ = nullptr;
    proxy_add_listener_t fn_add_listener_ = nullptr;
    proxy_destroy_t fn_proxy_destroy_ = nullptr;
    proxy_get_version_t fn_get_version_ = nullptr;

    const wl_interface* wl_registry_interface_ = nullptr;
    const wl_interface* wl_compositor_interface_ = nullptr;
    const wl_interface* wl_surface_interface_ = nullptr;

    explicit wayland_window(dl_lib lib) : lib_(std::move(lib)) {}

    bool load_functions();
    bool bind_globals();

    static void registry_global(void* data, wl_proxy* registry, uint32_t name,
                                const char* interface, uint32_t version);
    static void registry_global_remove(void* data, wl_proxy* registry, uint32_t name);
    static void wm_base_ping(void* data, wl_proxy* wm_base, uint32_t serial);
    static void xdg_surface_configure(void* data, wl_proxy* xdg_surface, uint32_t serial);
    static void toplevel_configure(void* data, wl_proxy* toplevel, int32_t width, int32_t height,
                                   wl_proxy* states);
    static void toplevel_close(void* data, wl_proxy* toplevel);
    static void toplevel_configure_bounds(void* data, wl_proxy* toplevel, int32_t width,
                                          int32_t height);
    static void toplevel_wm_capabilities(void* data, wl_proxy* toplevel, wl_proxy* capabilities);
};

}  // namespace mirage::platform
