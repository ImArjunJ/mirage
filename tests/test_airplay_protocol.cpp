#include <cmath>
#include <iostream>
#include <string>

#include "protocols/airplay_protocol.hpp"

namespace {

bool near(float actual, float expected, float tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace mirage::protocols;
    bool ok = true;

    ok &= expect(airplay::feature_txt() == "0x5A7FFEE6,0x0", "feature TXT mismatch");
    ok &= expect(airplay::compatibility_model == std::string("AppleTV6,2"), "model mismatch");
    ok &= expect(airplay::audio_latency_samples(44100) == 11025, "44100 latency mismatch");
    ok &= expect(airplay::audio_latency_samples(48000) == 12000, "48000 latency mismatch");
    ok &= expect(airplay::audio_latency_samples(0) == 11025, "default latency mismatch");

    ok &= expect(airplay::audio_seq_delta(10, 9) == 1, "forward sequence delta mismatch");
    ok &= expect(airplay::audio_seq_delta(9, 10) == -1, "late sequence delta mismatch");
    ok &= expect(airplay::audio_seq_delta(0, 65535) == 1, "wrap forward delta mismatch");
    ok &= expect(airplay::audio_seq_delta(65535, 0) == -1, "wrap late delta mismatch");
    ok &= expect(airplay::audio_seq_delta(1000, 1000) == 0, "equal sequence delta mismatch");

    ok &= expect(airplay::clamp_resend_count(-1) == 0, "negative resend clamp mismatch");
    ok &= expect(airplay::clamp_resend_count(0) == 0, "zero resend clamp mismatch");
    ok &= expect(airplay::clamp_resend_count(4) == 4, "small resend clamp mismatch");
    ok &= expect(airplay::clamp_resend_count(1000) == airplay::max_audio_resend_packets,
                 "large resend clamp mismatch");

    ok &= expect(near(airplay::db_to_linear(0.0F), 1.0F, 0.0001F),
                 "0 dB conversion mismatch");
    ok &= expect(near(airplay::db_to_linear(-6.0F), 0.5012F, 0.001F),
                 "-6 dB conversion mismatch");
    ok &= expect(airplay::db_to_linear(-30.0F) == 0.0F, "-30 dB mute mismatch");
    ok &= expect(airplay::db_to_linear(-90.0F) == 0.0F, "-90 dB mute mismatch");

    return ok ? 0 : 1;
}
