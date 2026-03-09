#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "media/media.hpp"
namespace mirage::render {
class render_window {
public:
    static std::unique_ptr<render_window> create(std::string_view title, uint32_t width,
                                                 uint32_t height);
    void submit_frame(media::decoded_frame frame);
    [[nodiscard]] bool is_open() const { return open_.load(std::memory_order_relaxed); }
    ~render_window();
    render_window(const render_window&) = delete;
    render_window& operator=(const render_window&) = delete;
    render_window(render_window&&) = delete;
    render_window& operator=(render_window&&) = delete;

private:
    render_window() = default;
    void thread_main(const std::string& title, uint32_t width, uint32_t height);
    std::thread thread_;
    std::atomic<bool> open_{false};
    std::atomic<bool> started_{false};
    std::mutex frame_mutex_;
    std::optional<media::decoded_frame> pending_frame_;
};
struct yuv_conversion_params {
    enum class color_space : uint8_t { bt601, bt709 };
    color_space space = color_space::bt709;
    bool full_range = false;
};
}  // namespace mirage::render
