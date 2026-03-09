#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

#include <X11/Xlib.h>

#include "dl.hpp"
#include "window.hpp"

namespace mirage::platform {

class x11_window : public window {
public:
    static std::unique_ptr<window> try_create(std::string_view title, uint32_t w, uint32_t h);

    ~x11_window() override;

    [[nodiscard]] std::vector<const char*> required_vulkan_extensions() const override;
    VkSurfaceKHR create_vulkan_surface(VkInstance instance) override;
    [[nodiscard]] std::pair<uint32_t, uint32_t> size_pixels() const override;

    bool poll_events() override;
    [[nodiscard]] bool resize_pending() override;
    void clear_resize_flag() override;
    void wait_events() override;

private:
    using open_display_t = Display* (*)(const char*);
    using close_display_t = int (*)(Display*);
    using create_simple_window_t = Window (*)(Display*, Window, int, int, unsigned int,
                                              unsigned int, unsigned int, unsigned long,
                                              unsigned long);
    using destroy_window_t = int (*)(Display*, Window);
    using map_window_t = int (*)(Display*, Window);
    using store_name_t = int (*)(Display*, Window, const char*);
    using select_input_t = int (*)(Display*, Window, long);
    using pending_t = int (*)(Display*);
    using next_event_t = int (*)(Display*, XEvent*);
    using intern_atom_t = Atom (*)(Display*, const char*, Bool);
    using set_wm_protocols_t = Status (*)(Display*, Window, Atom*, int);
    using get_window_attributes_t = Status (*)(Display*, Window, XWindowAttributes*);
    using flush_t = int (*)(Display*);

    dl_lib lib_;
    Display* display_ = nullptr;
    Window window_ = 0;
    Atom wm_delete_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool closed_ = false;
    bool resized_ = false;

    open_display_t fn_open_display_ = nullptr;
    close_display_t fn_close_display_ = nullptr;
    create_simple_window_t fn_create_simple_window_ = nullptr;
    destroy_window_t fn_destroy_window_ = nullptr;
    map_window_t fn_map_window_ = nullptr;
    store_name_t fn_store_name_ = nullptr;
    select_input_t fn_select_input_ = nullptr;
    pending_t fn_pending_ = nullptr;
    next_event_t fn_next_event_ = nullptr;
    intern_atom_t fn_intern_atom_ = nullptr;
    set_wm_protocols_t fn_set_wm_protocols_ = nullptr;
    get_window_attributes_t fn_get_window_attributes_ = nullptr;
    flush_t fn_flush_ = nullptr;

    explicit x11_window(dl_lib lib) : lib_(std::move(lib)) {}

    bool load_functions();
    void handle_event(const XEvent& ev);
};

}  // namespace mirage::platform
