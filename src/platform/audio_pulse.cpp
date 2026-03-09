#include "audio_pulse.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace mirage::platform {

std::unique_ptr<audio_output> pulse_output::try_create(int sample_rate, int channels,
                                                       fill_callback cb) {
    auto out = std::unique_ptr<pulse_output>(new pulse_output());

    out->lib_ = dl_lib::open("libpulse-simple.so.0");
    if (!out->lib_.loaded()) {
        out->lib_ = dl_lib::open("libpulse-simple.so");
    }
    if (!out->lib_.loaded()) {
        return nullptr;
    }

    out->fn_new_ = out->lib_.sym<pa_simple_new_fn>("pa_simple_new");
    out->fn_write_ = out->lib_.sym<pa_simple_write_fn>("pa_simple_write");
    out->fn_drain_ = out->lib_.sym<pa_simple_drain_fn>("pa_simple_drain");
    out->fn_free_ = out->lib_.sym<pa_simple_free_fn>("pa_simple_free");

    if (!out->fn_new_ || !out->fn_write_ || !out->fn_drain_ || !out->fn_free_) {
        return nullptr;
    }

    pa_sample_spec spec{
        .format = PA_SAMPLE_S16LE,
        .rate = static_cast<uint32_t>(sample_rate),
        .channels = static_cast<uint8_t>(channels),
    };

    int error = 0;
    out->pa_ = out->fn_new_(nullptr, "mirage", PA_STREAM_PLAYBACK, nullptr, "audio", &spec, nullptr,
                            nullptr, &error);
    if (!out->pa_) {
        return nullptr;
    }

    out->callback_ = std::move(cb);
    out->running_.store(true, std::memory_order_relaxed);

    out->thread_ = std::thread([raw = out.get()] {
        std::array<std::byte, 4096> buf{};
        while (raw->running_.load(std::memory_order_relaxed)) {
            auto filled = raw->callback_(buf);
            if (filled == 0) {
                continue;
            }

            float vol = raw->volume_.load(std::memory_order_relaxed);
            auto* samples = reinterpret_cast<int16_t*>(buf.data());
            size_t sample_count = filled / sizeof(int16_t);
            for (size_t i = 0; i < sample_count; ++i) {
                samples[i] = static_cast<int16_t>(
                    std::clamp(static_cast<float>(samples[i]) * vol, -32768.0f, 32767.0f));
            }

            int err = 0;
            raw->fn_write_(raw->pa_, buf.data(), filled, &err);
        }
    });

    return out;
}

void pulse_output::set_volume(float linear) {
    volume_.store(linear, std::memory_order_relaxed);
}

void pulse_output::stop() {
    if (running_.exchange(false, std::memory_order_relaxed)) {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (pa_) {
            int err = 0;
            fn_drain_(pa_, &err);
            fn_free_(pa_);
            pa_ = nullptr;
        }
    }
}

pulse_output::~pulse_output() {
    stop();
}

}  // namespace mirage::platform
