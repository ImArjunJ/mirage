#pragma once
#ifdef _WIN32
#include <atomic>
#include <thread>

#include "audio_out.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>

namespace mirage::platform {

class wasapi_output : public audio_output {
public:
    static std::unique_ptr<audio_output> try_create(int sample_rate, int channels,
                                                    fill_callback cb);
    void set_volume(float linear) override;
    void stop() override;
    ~wasapi_output() override;

private:
    wasapi_output() = default;

    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<float> volume_{1.0f};
    std::thread thread_;
    fill_callback callback_;
    UINT32 buffer_frames_ = 0;
    int channels_ = 2;
};

}  // namespace mirage::platform
#endif
