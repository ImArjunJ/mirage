#ifdef _WIN32
#include "window_win32.hpp"

#include <string>

#include <vulkan/vulkan_win32.h>

namespace mirage::platform {

static constexpr wchar_t kClassName[] = L"MirageWindowClass";

LRESULT CALLBACK win32_window::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    auto* self = reinterpret_cast<win32_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_CLOSE:
            self->closed_ = true;
            return 0;
        case WM_SIZE: {
            auto w = static_cast<uint32_t>(LOWORD(lp));
            auto h = static_cast<uint32_t>(HIWORD(lp));
            if (w != self->width_ || h != self->height_) {
                self->width_ = w;
                self->height_ = h;
                self->resized_ = true;
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

std::unique_ptr<window> win32_window::try_create(std::string_view title, uint32_t w, uint32_t h) {
    auto win = std::unique_ptr<win32_window>(new win32_window());

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;

    win->class_atom_ = RegisterClassExW(&wc);
    if (!win->class_atom_) {
        return nullptr;
    }

    RECT rect{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);

    int ww = rect.right - rect.left;
    int wh = rect.bottom - rect.top;

    std::wstring wtitle(title.begin(), title.end());

    win->hwnd_ = CreateWindowExW(0, kClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, ww, wh, nullptr, nullptr, hinst, win.get());

    if (!win->hwnd_) {
        UnregisterClassW(kClassName, hinst);
        win->class_atom_ = 0;
        return nullptr;
    }

    win->width_ = w;
    win->height_ = h;

    ShowWindow(win->hwnd_, SW_SHOW);
    UpdateWindow(win->hwnd_);

    return win;
}

win32_window::~win32_window() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_atom_) {
        UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
        class_atom_ = 0;
    }
}

std::vector<const char*> win32_window::required_vulkan_extensions() const {
    return {"VK_KHR_surface", "VK_KHR_win32_surface"};
}

VkSurfaceKHR win32_window::create_vulkan_surface(VkInstance instance) {
    VkWin32SurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = GetModuleHandleW(nullptr);
    info.hwnd = hwnd_;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vkCreateWin32SurfaceKHR(instance, &info, nullptr, &surface);
    return surface;
}

std::pair<uint32_t, uint32_t> win32_window::size_pixels() const {
    return {width_, height_};
}

bool win32_window::poll_events() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

bool win32_window::resize_pending() {
    return resized_;
}

void win32_window::clear_resize_flag() {
    resized_ = false;
}

void win32_window::wait_events() {
    MSG msg{};
    if (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace mirage::platform
#endif
