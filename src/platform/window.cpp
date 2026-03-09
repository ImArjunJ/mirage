#include "platform/window.hpp"

#ifdef _WIN32
#include "platform/window_win32.hpp"
#else
#include "platform/window_wayland.hpp"
#include "platform/window_x11.hpp"
#endif

#include "core/log.hpp"

namespace mirage::platform {

std::unique_ptr<window> window::create(std::string_view title, uint32_t w, uint32_t h) {
#ifdef _WIN32
    if (auto win = win32_window::try_create(title, w, h)) {
        mirage::log::info("using win32 window backend");
        return win;
    }
#else
    if (auto wl = wayland_window::try_create(title, w, h)) {
        mirage::log::info("using wayland window backend");
        return wl;
    }
    if (auto x = x11_window::try_create(title, w, h)) {
        mirage::log::info("using x11 window backend");
        return x;
    }
#endif
    mirage::log::error("no window backend available");
    return nullptr;
}

}  // namespace mirage::platform
