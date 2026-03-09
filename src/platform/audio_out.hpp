#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace mirage::platform {

class audio_output {
public:
    using fill_callback = std::function<size_t(std::span<std::byte> buf)>;

    static std::unique_ptr<audio_output> create(int sample_rate, int channels, fill_callback cb);

    virtual ~audio_output() = default;
    virtual void set_volume(float linear) = 0;
    virtual void stop() = 0;

    audio_output() = default;
    audio_output(const audio_output&) = delete;
    audio_output& operator=(const audio_output&) = delete;
};

}  // namespace mirage::platform
