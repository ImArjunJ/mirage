#pragma once
#include "audio_out.hpp"

namespace mirage::platform {

std::unique_ptr<audio_output> create_alsa_output(int sample_rate, int channels,
                                                 audio_output::fill_callback cb);

}
