#include "window_x11.hpp"

#include <cstring>
#include <string>

#include <vulkan/vulkan_xlib.h>

namespace mirage::platform {

bool x11_window::load_functions() {
    fn_open_display_ = lib_.sym<open_display_t>("XOpenDisplay");
    fn_close_display_ = lib_.sym<close_display_t>("XCloseDisplay");
    fn_create_simple_window_ = lib_.sym<create_simple_window_t>("XCreateSimpleWindow");
    fn_destroy_window_ = lib_.sym<destroy_window_t>("XDestroyWindow");
    fn_map_window_ = lib_.sym<map_window_t>("XMapWindow");
    fn_store_name_ = lib_.sym<store_name_t>("XStoreName");
    fn_select_input_ = lib_.sym<select_input_t>("XSelectInput");
    fn_pending_ = lib_.sym<pending_t>("XPending");
    fn_next_event_ = lib_.sym<next_event_t>("XNextEvent");
    fn_intern_atom_ = lib_.sym<intern_atom_t>("XInternAtom");
    fn_set_wm_protocols_ = lib_.sym<set_wm_protocols_t>("XSetWMProtocols");
    fn_get_window_attributes_ = lib_.sym<get_window_attributes_t>("XGetWindowAttributes");
    fn_flush_ = lib_.sym<flush_t>("XFlush");

    return fn_open_display_ && fn_close_display_ && fn_create_simple_window_ &&
           fn_destroy_window_ && fn_map_window_ && fn_store_name_ && fn_select_input_ &&
           fn_pending_ && fn_next_event_ && fn_intern_atom_ && fn_set_wm_protocols_ &&
           fn_get_window_attributes_ && fn_flush_;
}

std::unique_ptr<window> x11_window::try_create(std::string_view title, uint32_t w, uint32_t h) {
    auto lib = dl_lib::open("libX11.so.6");
    if (!lib.loaded()) {
        lib = dl_lib::open("libX11.so");
    }
    if (!lib.loaded()) {
        return nullptr;
    }

    auto win = std::unique_ptr<x11_window>(new x11_window(std::move(lib)));
    if (!win->load_functions()) {
        return nullptr;
    }

    win->display_ = win->fn_open_display_(nullptr);
    if (!win->display_) {
        return nullptr;
    }

    Window root = DefaultRootWindow(win->display_);
    win->width_ = w;
    win->height_ = h;
    win->window_ = win->fn_create_simple_window_(win->display_, root, 0, 0, w, h, 0, 0, 0);

    std::string title_str(title);
    win->fn_store_name_(win->display_, win->window_, title_str.c_str());
    win->fn_select_input_(win->display_, win->window_,
                          StructureNotifyMask | KeyPressMask | ExposureMask);

    win->wm_delete_ = win->fn_intern_atom_(win->display_, "WM_DELETE_WINDOW", False);
    win->fn_set_wm_protocols_(win->display_, win->window_, &win->wm_delete_, 1);

    win->fn_map_window_(win->display_, win->window_);
    win->fn_flush_(win->display_);

    return win;
}

x11_window::~x11_window() {
    if (display_) {
        if (window_) {
            fn_destroy_window_(display_, window_);
        }
        fn_close_display_(display_);
    }
}

std::vector<const char*> x11_window::required_vulkan_extensions() const {
    return {"VK_KHR_surface", "VK_KHR_xlib_surface"};
}

VkSurfaceKHR x11_window::create_vulkan_surface(VkInstance instance) {
    VkXlibSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = display_;
    info.window = window_;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkCreateXlibSurfaceKHR(instance, &info, nullptr, &surface);
    return surface;
}

std::pair<uint32_t, uint32_t> x11_window::size_pixels() const {
    return {width_, height_};
}

void x11_window::handle_event(const XEvent& ev) {
    if (ev.type == ConfigureNotify) {
        auto w = static_cast<uint32_t>(ev.xconfigure.width);
        auto h = static_cast<uint32_t>(ev.xconfigure.height);
        if (w != width_ || h != height_) {
            width_ = w;
            height_ = h;
            resized_ = true;
        }
    } else if (ev.type == ClientMessage) {
        if (static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete_) {
            closed_ = true;
        }
    }
}

bool x11_window::poll_events() {
    while (fn_pending_(display_) > 0) {
        XEvent ev;
        fn_next_event_(display_, &ev);
        handle_event(ev);
    }
    return !closed_;
}

bool x11_window::resize_pending() {
    return resized_;
}

void x11_window::clear_resize_flag() {
    resized_ = false;
}

void x11_window::wait_events() {
    XEvent ev;
    fn_next_event_(display_, &ev);
    handle_event(ev);

    while (fn_pending_(display_) > 0) {
        fn_next_event_(display_, &ev);
        handle_event(ev);
    }
}

}  // namespace mirage::platform
