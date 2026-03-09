#include "window_wayland.hpp"

#include <cstring>
#include <string>

#include <poll.h>

struct wl_surface;

#include <vulkan/vulkan_wayland.h>

namespace mirage::platform {

namespace {

const wl_interface* no_iface[] = {nullptr};

wl_interface xdg_positioner_interface_stub = {"xdg_positioner", 1, 0, nullptr, 0, nullptr};
wl_interface xdg_toplevel_interface = {};
wl_interface xdg_surface_interface = {};

const wl_interface* create_positioner_types[] = {&xdg_positioner_interface_stub};
const wl_interface* get_xdg_surface_types[] = {&xdg_surface_interface, nullptr};
const wl_interface* get_toplevel_types[] = {&xdg_toplevel_interface};

const wl_message xdg_wm_base_requests[] = {
    {"destroy", "", no_iface},
    {"create_positioner", "n", create_positioner_types},
    {"get_xdg_surface", "no", get_xdg_surface_types},
    {"pong", "u", no_iface},
};

const wl_message xdg_wm_base_events[] = {
    {"ping", "u", no_iface},
};

wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, xdg_wm_base_requests, 1, xdg_wm_base_events,
};

const wl_message xdg_surface_requests[] = {
    {"destroy", "", no_iface},        {"get_toplevel", "n", get_toplevel_types},
    {"get_popup", "n?oo", no_iface},  {"set_window_geometry", "iiii", no_iface},
    {"ack_configure", "u", no_iface},
};

const wl_message xdg_surface_events[] = {
    {"configure", "u", no_iface},
};

const wl_message xdg_toplevel_requests[] = {
    {"destroy", "", no_iface},
    {"set_parent", "?o", no_iface},
    {"set_title", "s", no_iface},
    {"set_app_id", "s", no_iface},
    {"show_window_menu", "ouii", no_iface},
    {"move", "ou", no_iface},
    {"resize", "oui", no_iface},
    {"set_max_size", "ii", no_iface},
    {"set_min_size", "ii", no_iface},
    {"set_maximized", "", no_iface},
    {"unset_maximized", "", no_iface},
    {"set_fullscreen", "?o", no_iface},
    {"unset_fullscreen", "", no_iface},
    {"set_minimized", "", no_iface},
};

const wl_message xdg_toplevel_events[] = {
    {"configure", "iia", no_iface},
    {"close", "", no_iface},
    {"configure_bounds", "ii", no_iface},
    {"wm_capabilities", "a", no_iface},
};

void init_protocol_interfaces() {
    xdg_surface_interface = {
        "xdg_surface", 1, 5, xdg_surface_requests, 1, xdg_surface_events,
    };
    xdg_toplevel_interface = {
        "xdg_toplevel", 1, 14, xdg_toplevel_requests, 4, xdg_toplevel_events,
    };
}

}  // namespace

bool wayland_window::load_functions() {
    fn_connect_ = lib_.sym<display_connect_t>("wl_display_connect");
    fn_disconnect_ = lib_.sym<display_disconnect_t>("wl_display_disconnect");
    fn_dispatch_ = lib_.sym<display_dispatch_t>("wl_display_dispatch");
    fn_dispatch_pending_ = lib_.sym<display_dispatch_pending_t>("wl_display_dispatch_pending");
    fn_roundtrip_ = lib_.sym<display_roundtrip_t>("wl_display_roundtrip");
    fn_flush_ = lib_.sym<display_flush_t>("wl_display_flush");
    fn_get_fd_ = lib_.sym<display_get_fd_t>("wl_display_get_fd");
    fn_prepare_read_ = lib_.sym<display_prepare_read_t>("wl_display_prepare_read");
    fn_read_events_ = lib_.sym<display_read_events_t>("wl_display_read_events");
    fn_cancel_read_ = lib_.sym<display_cancel_read_t>("wl_display_cancel_read");
    fn_marshal_ = lib_.sym<proxy_marshal_flags_t>("wl_proxy_marshal_flags");
    fn_add_listener_ = lib_.sym<proxy_add_listener_t>("wl_proxy_add_listener");
    fn_proxy_destroy_ = lib_.sym<proxy_destroy_t>("wl_proxy_destroy");
    fn_get_version_ = lib_.sym<proxy_get_version_t>("wl_proxy_get_version");

    wl_registry_interface_ = lib_.sym<const wl_interface*>("wl_registry_interface");
    wl_compositor_interface_ = lib_.sym<const wl_interface*>("wl_compositor_interface");
    wl_surface_interface_ = lib_.sym<const wl_interface*>("wl_surface_interface");

    return fn_connect_ && fn_disconnect_ && fn_dispatch_ && fn_dispatch_pending_ && fn_roundtrip_ &&
           fn_flush_ && fn_get_fd_ && fn_prepare_read_ && fn_read_events_ && fn_cancel_read_ &&
           fn_marshal_ && fn_add_listener_ && fn_proxy_destroy_ && fn_get_version_ &&
           wl_registry_interface_ && wl_compositor_interface_ && wl_surface_interface_;
}

void wayland_window::registry_global(void* data, wl_proxy* registry, uint32_t name,
                                     const char* interface, uint32_t version) {
    auto* self = static_cast<wayland_window*>(data);

    if (std::strcmp(interface, "wl_compositor") == 0) {
        self->compositor_ = self->fn_marshal_(
            registry, 0, self->wl_compositor_interface_, version > 6 ? 6 : version, 0, name,
            self->wl_compositor_interface_->name, version > 6 ? 6 : version, nullptr);
    } else if (std::strcmp(interface, "xdg_wm_base") == 0) {
        self->wm_base_ =
            self->fn_marshal_(registry, 0, &xdg_wm_base_interface, version > 1 ? 1 : version, 0,
                              name, xdg_wm_base_interface.name, version > 1 ? 1 : version, nullptr);

        static void (*wm_base_listener[])(void) = {
            reinterpret_cast<void (*)(void)>(wm_base_ping),
        };
        self->fn_add_listener_(self->wm_base_, wm_base_listener, self);
    }
}

void wayland_window::registry_global_remove(void* /*data*/, wl_proxy* /*registry*/,
                                            uint32_t /*name*/) {}

void wayland_window::wm_base_ping(void* data, wl_proxy* wm_base, uint32_t serial) {
    auto* self = static_cast<wayland_window*>(data);
    self->fn_marshal_(wm_base, 3, nullptr, self->fn_get_version_(wm_base), 0, serial);
}

void wayland_window::xdg_surface_configure(void* data, wl_proxy* xdg_surface, uint32_t serial) {
    auto* self = static_cast<wayland_window*>(data);
    self->fn_marshal_(xdg_surface, 4, nullptr, self->fn_get_version_(xdg_surface), 0, serial);
    self->configured_ = true;
}

void wayland_window::toplevel_configure(void* data, wl_proxy* /*toplevel*/, int32_t width,
                                        int32_t height, wl_proxy* /*states*/) {
    auto* self = static_cast<wayland_window*>(data);
    if (width > 0 && height > 0) {
        auto w = static_cast<uint32_t>(width);
        auto h = static_cast<uint32_t>(height);
        if (w != self->width_ || h != self->height_) {
            self->width_ = w;
            self->height_ = h;
            self->resized_ = true;
        }
    }
}

void wayland_window::toplevel_close(void* data, wl_proxy* /*toplevel*/) {
    auto* self = static_cast<wayland_window*>(data);
    self->closed_ = true;
}

void wayland_window::toplevel_configure_bounds(void* /*data*/, wl_proxy* /*toplevel*/,
                                               int32_t /*width*/, int32_t /*height*/) {}

void wayland_window::toplevel_wm_capabilities(void* /*data*/, wl_proxy* /*toplevel*/,
                                              wl_proxy* /*capabilities*/) {}

bool wayland_window::bind_globals() {
    auto* display_proxy = reinterpret_cast<wl_proxy*>(display_);
    registry_ = fn_marshal_(display_proxy, 1, wl_registry_interface_,
                            fn_get_version_(display_proxy), 0, nullptr);
    if (!registry_) {
        return false;
    }

    static void (*registry_listener[])(void) = {
        reinterpret_cast<void (*)(void)>(registry_global),
        reinterpret_cast<void (*)(void)>(registry_global_remove),
    };
    fn_add_listener_(registry_, registry_listener, this);
    fn_roundtrip_(display_);
    fn_roundtrip_(display_);

    return compositor_ && wm_base_;
}

std::unique_ptr<window> wayland_window::try_create(std::string_view title, uint32_t w, uint32_t h) {
    init_protocol_interfaces();

    auto lib = dl_lib::open("libwayland-client.so.0");
    if (!lib.loaded()) {
        lib = dl_lib::open("libwayland-client.so");
    }
    if (!lib.loaded()) {
        return nullptr;
    }

    auto win = std::unique_ptr<wayland_window>(new wayland_window(std::move(lib)));
    if (!win->load_functions()) {
        return nullptr;
    }

    win->display_ = win->fn_connect_(nullptr);
    if (!win->display_) {
        return nullptr;
    }

    win->width_ = w;
    win->height_ = h;

    if (!win->bind_globals()) {
        return nullptr;
    }

    win->surface_ = win->fn_marshal_(win->compositor_, 0, win->wl_surface_interface_,
                                     win->fn_get_version_(win->compositor_), 0, nullptr);
    if (!win->surface_) {
        return nullptr;
    }

    win->xdg_surface_ =
        win->fn_marshal_(win->wm_base_, 2, &xdg_surface_interface,
                         win->fn_get_version_(win->wm_base_), 0, nullptr, win->surface_);
    if (!win->xdg_surface_) {
        return nullptr;
    }

    static void (*xdg_surface_listener[])(void) = {
        reinterpret_cast<void (*)(void)>(xdg_surface_configure),
    };
    win->fn_add_listener_(win->xdg_surface_, xdg_surface_listener, win.get());

    win->toplevel_ = win->fn_marshal_(win->xdg_surface_, 1, &xdg_toplevel_interface,
                                      win->fn_get_version_(win->xdg_surface_), 0, nullptr);
    if (!win->toplevel_) {
        return nullptr;
    }

    static void (*toplevel_listener[])(void) = {
        reinterpret_cast<void (*)(void)>(toplevel_configure),
        reinterpret_cast<void (*)(void)>(toplevel_close),
        reinterpret_cast<void (*)(void)>(toplevel_configure_bounds),
        reinterpret_cast<void (*)(void)>(toplevel_wm_capabilities),
    };
    win->fn_add_listener_(win->toplevel_, toplevel_listener, win.get());

    std::string title_str(title);
    win->fn_marshal_(win->toplevel_, 2, nullptr, win->fn_get_version_(win->toplevel_), 0,
                     title_str.c_str());

    win->fn_marshal_(win->surface_, 6, nullptr, win->fn_get_version_(win->surface_), 0);

    win->fn_roundtrip_(win->display_);

    return win;
}

wayland_window::~wayland_window() {
    if (toplevel_) {
        fn_proxy_destroy_(toplevel_);
    }
    if (xdg_surface_) {
        fn_proxy_destroy_(xdg_surface_);
    }
    if (wm_base_) {
        fn_proxy_destroy_(wm_base_);
    }
    if (surface_) {
        fn_proxy_destroy_(surface_);
    }
    if (compositor_) {
        fn_proxy_destroy_(compositor_);
    }
    if (registry_) {
        fn_proxy_destroy_(registry_);
    }
    if (display_) {
        fn_disconnect_(display_);
    }
}

std::vector<const char*> wayland_window::required_vulkan_extensions() const {
    return {"VK_KHR_surface", "VK_KHR_wayland_surface"};
}

VkSurfaceKHR wayland_window::create_vulkan_surface(VkInstance instance) {
    VkWaylandSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    info.display = display_;
    info.surface = reinterpret_cast<struct wl_surface*>(surface_);

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    vkCreateWaylandSurfaceKHR(instance, &info, nullptr, &vk_surface);
    return vk_surface;
}

std::pair<uint32_t, uint32_t> wayland_window::size_pixels() const {
    return {width_, height_};
}

bool wayland_window::poll_events() {
    while (fn_prepare_read_(display_) != 0) {
        fn_dispatch_pending_(display_);
    }
    fn_flush_(display_);
    struct pollfd pfd{fn_get_fd_(display_), POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0) {
        fn_read_events_(display_);
    } else {
        fn_cancel_read_(display_);
    }
    fn_dispatch_pending_(display_);
    return !closed_;
}

bool wayland_window::resize_pending() {
    return resized_;
}

void wayland_window::clear_resize_flag() {
    resized_ = false;
}

void wayland_window::wait_events() {
    fn_dispatch_(display_);
}

}  // namespace mirage::platform
