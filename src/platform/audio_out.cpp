#include "platform/audio_out.hpp"

#ifdef _WIN32
#include "platform/audio_wasapi.hpp"
#else
#include "platform/audio_alsa.hpp"
#include "platform/audio_pulse.hpp"
#endif

#include "core/log.hpp"

namespace mirage::platform {

std::unique_ptr<audio_output> audio_output::create(int sample_rate, int channels,
                                                   fill_callback cb) {
#ifdef _WIN32
    if (auto w = wasapi_output::try_create(sample_rate, channels, std::move(cb))) {
        mirage::log::info("using wasapi audio backend");
        return w;
    }
#else
    if (auto pa = pulse_output::try_create(sample_rate, channels, cb)) {
        mirage::log::info("using pulseaudio audio backend");
        return pa;
    }
    if (auto alsa = create_alsa_output(sample_rate, channels, std::move(cb))) {
        mirage::log::info("using alsa audio backend");
        return alsa;
    }
#endif
    mirage::log::error("no audio backend available");
    return nullptr;
}

}  // namespace mirage::platform
