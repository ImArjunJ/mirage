#ifdef _WIN32
#include "audio_wasapi.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace mirage::platform {

static constexpr REFERENCE_TIME kBufferDuration = 500000;

std::unique_ptr<audio_output> wasapi_output::try_create(int sample_rate, int channels,
                                                        fill_callback cb) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return nullptr;
    }

    auto out = std::unique_ptr<wasapi_output>(new wasapi_output());
    out->channels_ = channels;
    out->callback_ = std::move(cb);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&out->enumerator_));
    if (FAILED(hr) || !out->enumerator_) {
        return nullptr;
    }

    hr = out->enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &out->device_);
    if (FAILED(hr) || !out->device_) {
        return nullptr;
    }

    hr = out->device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&out->client_));
    if (FAILED(hr) || !out->client_) {
        return nullptr;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = static_cast<DWORD>(sample_rate) * format.nBlockAlign;
    format.cbSize = 0;

    hr = out->client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        kBufferDuration, 0, &format, nullptr);
    if (FAILED(hr)) {
        return nullptr;
    }

    hr = out->client_->GetBufferSize(&out->buffer_frames_);
    if (FAILED(hr)) {
        return nullptr;
    }

    hr = out->client_->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&out->render_));
    if (FAILED(hr) || !out->render_) {
        return nullptr;
    }

    hr = out->client_->Start();
    if (FAILED(hr)) {
        return nullptr;
    }

    out->running_.store(true, std::memory_order_relaxed);

    out->thread_ = std::thread([raw = out.get()] {
        while (raw->running_.load(std::memory_order_relaxed)) {
            UINT32 padding = 0;
            HRESULT hr = raw->client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                break;
            }

            UINT32 available = raw->buffer_frames_ - padding;
            if (available == 0) {
                Sleep(5);
                continue;
            }

            BYTE* data = nullptr;
            hr = raw->render_->GetBuffer(available, &data);
            if (FAILED(hr)) {
                break;
            }

            size_t bytes = static_cast<size_t>(available) * static_cast<size_t>(raw->channels_) *
                           sizeof(int16_t);
            auto filled =
                raw->callback_(std::span<std::byte>(reinterpret_cast<std::byte*>(data), bytes));

            if (filled < bytes) {
                std::memset(data + filled, 0, bytes - filled);
            }

            float vol = raw->volume_.load(std::memory_order_relaxed);
            auto* samples = reinterpret_cast<int16_t*>(data);
            size_t sample_count = bytes / sizeof(int16_t);
            for (size_t i = 0; i < sample_count; ++i) {
                samples[i] = static_cast<int16_t>(
                    std::clamp(static_cast<float>(samples[i]) * vol, -32768.0f, 32767.0f));
            }

            raw->render_->ReleaseBuffer(available, 0);
            Sleep(5);
        }
    });

    return out;
}

void wasapi_output::set_volume(float linear) {
    volume_.store(linear, std::memory_order_relaxed);
}

void wasapi_output::stop() {
    if (running_.exchange(false, std::memory_order_relaxed)) {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (client_) {
            client_->Stop();
        }
    }
    if (render_) {
        render_->Release();
        render_ = nullptr;
    }
    if (client_) {
        client_->Release();
        client_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
}

wasapi_output::~wasapi_output() {
    stop();
}

}  // namespace mirage::platform
#endif
