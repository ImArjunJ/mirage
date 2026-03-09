#pragma once
#include <atomic>
#include <thread>

#include "audio_out.hpp"
#include "dl.hpp"

namespace mirage::platform {

enum pa_sample_format_t { PA_SAMPLE_S16LE = 3 };

struct pa_sample_spec {
    pa_sample_format_t format;
    uint32_t rate;
    uint8_t channels;
};

enum pa_stream_direction_t { PA_STREAM_PLAYBACK = 1 };

using pa_simple = void;

class pulse_output : public audio_output {
public:
    static std::unique_ptr<audio_output> try_create(int sample_rate, int channels,
                                                    fill_callback cb);

    void set_volume(float linear) override;
    void stop() override;
    ~pulse_output() override;

private:
    pulse_output() = default;

    dl_lib lib_{dl_lib::open(nullptr)};
    pa_simple* pa_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<float> volume_{1.0f};
    std::thread thread_;
    fill_callback callback_;

    using pa_simple_new_fn = pa_simple* (*)(const char*, const char*, pa_stream_direction_t,
                                            const char*, const char*, const pa_sample_spec*,
                                            const void*, const void*, int*);
    using pa_simple_write_fn = int (*)(pa_simple*, const void*, size_t, int*);
    using pa_simple_drain_fn = int (*)(pa_simple*, int*);
    using pa_simple_free_fn = void (*)(pa_simple*);

    pa_simple_new_fn fn_new_ = nullptr;
    pa_simple_write_fn fn_write_ = nullptr;
    pa_simple_drain_fn fn_drain_ = nullptr;
    pa_simple_free_fn fn_free_ = nullptr;
};

}  // namespace mirage::platform
