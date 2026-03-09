#include "audio_alsa.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "dl.hpp"

namespace mirage::platform {

using snd_pcm_t = void;
enum snd_pcm_stream_t { SND_PCM_STREAM_PLAYBACK = 0 };
enum snd_pcm_format_t { SND_PCM_FORMAT_S16_LE = 2 };
enum snd_pcm_access_t { SND_PCM_ACCESS_RW_INTERLEAVED = 3 };
using snd_pcm_sframes_t = long;
using snd_pcm_uframes_t = unsigned long;

using fn_open = int (*)(snd_pcm_t**, const char*, snd_pcm_stream_t, int);
using fn_close = int (*)(snd_pcm_t*);
using fn_set_params = int (*)(snd_pcm_t*, snd_pcm_format_t, snd_pcm_access_t, unsigned, unsigned,
                              int, unsigned);
using fn_writei = snd_pcm_sframes_t (*)(snd_pcm_t*, const void*, snd_pcm_uframes_t);
using fn_prepare = int (*)(snd_pcm_t*);
using fn_recover = int (*)(snd_pcm_t*, int, int);

class alsa_output : public audio_output {
public:
    static std::unique_ptr<audio_output> try_create(int sample_rate, int channels,
                                                    fill_callback cb);
    void set_volume(float linear) override;
    void stop() override;
    ~alsa_output() override;

private:
    alsa_output() = default;

    dl_lib lib_{dl_lib::open(nullptr)};
    snd_pcm_t* pcm_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<float> volume_{1.0f};
    std::thread thread_;
    fill_callback callback_;
    int channels_ = 2;

    fn_close pcm_close_ = nullptr;
    fn_writei pcm_writei_ = nullptr;
    fn_prepare pcm_prepare_ = nullptr;
    fn_recover pcm_recover_ = nullptr;
};

std::unique_ptr<audio_output> alsa_output::try_create(int sample_rate, int channels,
                                                      fill_callback cb) {
    auto out = std::unique_ptr<alsa_output>(new alsa_output);

    out->lib_ = dl_lib::open("libasound.so.2");
    if (!out->lib_.loaded()) {
        out->lib_ = dl_lib::open("libasound.so");
    }
    if (!out->lib_.loaded()) {
        return nullptr;
    }

    auto pcm_open = out->lib_.sym<fn_open>("snd_pcm_open");
    auto pcm_set_params = out->lib_.sym<fn_set_params>("snd_pcm_set_params");
    out->pcm_close_ = out->lib_.sym<fn_close>("snd_pcm_close");
    out->pcm_writei_ = out->lib_.sym<fn_writei>("snd_pcm_writei");
    out->pcm_prepare_ = out->lib_.sym<fn_prepare>("snd_pcm_prepare");
    out->pcm_recover_ = out->lib_.sym<fn_recover>("snd_pcm_recover");

    if (!pcm_open || !pcm_set_params || !out->pcm_close_ || !out->pcm_writei_ ||
        !out->pcm_prepare_ || !out->pcm_recover_) {
        return nullptr;
    }

    if (pcm_open(&out->pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        return nullptr;
    }

    if (pcm_set_params(out->pcm_, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       static_cast<unsigned>(channels), static_cast<unsigned>(sample_rate), 1,
                       50000) < 0) {
        out->pcm_close_(out->pcm_);
        return nullptr;
    }

    out->channels_ = channels;
    out->callback_ = std::move(cb);
    out->running_ = true;

    out->thread_ = std::thread([raw = out.get()] {
        constexpr snd_pcm_uframes_t frame_count = 2048;
        const size_t buf_bytes =
            frame_count * static_cast<size_t>(raw->channels_) * sizeof(int16_t);
        std::vector<std::byte> buf(buf_bytes);

        while (raw->running_) {
            size_t filled = raw->callback_(buf);
            if (filled == 0) {
                continue;
            }

            float vol = raw->volume_.load(std::memory_order_relaxed);
            auto* samples = reinterpret_cast<int16_t*>(buf.data());
            size_t sample_count = filled / sizeof(int16_t);
            for (size_t i = 0; i < sample_count; ++i) {
                samples[i] = static_cast<int16_t>(static_cast<float>(samples[i]) * vol);
            }

            snd_pcm_uframes_t frames =
                filled / (static_cast<size_t>(raw->channels_) * sizeof(int16_t));
            snd_pcm_sframes_t written = raw->pcm_writei_(raw->pcm_, buf.data(), frames);
            if (written < 0) {
                raw->pcm_recover_(raw->pcm_, static_cast<int>(written), 0);
                raw->pcm_prepare_(raw->pcm_);
            }
        }
    });

    return out;
}

void alsa_output::set_volume(float linear) {
    volume_.store(linear, std::memory_order_relaxed);
}

void alsa_output::stop() {
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (pcm_) {
        pcm_close_(pcm_);
        pcm_ = nullptr;
    }
}

alsa_output::~alsa_output() {
    stop();
}

std::unique_ptr<audio_output> create_alsa_output(int sample_rate, int channels,
                                                 audio_output::fill_callback cb) {
    return alsa_output::try_create(sample_rate, channels, std::move(cb));
}

}  // namespace mirage::platform
