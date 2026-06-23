#include <iostream>
#include <string_view>

#include "protocols/airplay/rtsp_state.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool allowed(mirage::protocols::airplay::rtsp_session_state state,
             mirage::protocols::airplay::rtsp_action action) {
    return mirage::protocols::airplay::rtsp_action_allowed(state, action);
}

}  // namespace

int main() {
    using namespace mirage::protocols::airplay;
    bool ok = true;

    ok &= expect(classify_rtsp_action("GET", "/info") == rtsp_action::info,
                 "GET /info classification mismatch");
    ok &= expect(classify_rtsp_action("POST", "/fp-setup") == rtsp_action::fairplay_setup,
                 "FairPlay classification mismatch");
    ok &= expect(classify_rtsp_action("POST", "/command") == rtsp_action::control,
                 "command classification mismatch");
    ok &= expect(classify_rtsp_action("POST", "/feedback") == rtsp_action::control,
                 "feedback classification mismatch");
    ok &= expect(classify_rtsp_action("POST", "/audioMode") == rtsp_action::control,
                 "audio mode classification mismatch");

    ok &= expect(allowed(rtsp_session_state::init, rtsp_action::options),
                 "OPTIONS should be allowed before pairing");
    ok &= expect(allowed(rtsp_session_state::init, rtsp_action::info),
                 "GET /info should be allowed before pairing");
    ok &= expect(allowed(rtsp_session_state::init, rtsp_action::pair_setup),
                 "pair setup should be allowed from init");
    ok &= expect(allowed(rtsp_session_state::init, rtsp_action::pair_verify),
                 "direct pair verify should be allowed from init");
    ok &= expect(!allowed(rtsp_session_state::init, rtsp_action::setup),
                 "SETUP should be rejected before pairing");
    ok &= expect(!allowed(rtsp_session_state::init, rtsp_action::record),
                 "RECORD should be rejected before SETUP");
    ok &= expect(allowed(rtsp_session_state::init, rtsp_action::announce),
                 "RAOP ANNOUNCE should be allowed from init");

    ok &= expect(allowed(rtsp_session_state::pair_setup, rtsp_action::pair_verify),
                 "pair verify should be allowed after pair setup");
    ok &= expect(!allowed(rtsp_session_state::pair_verify, rtsp_action::record),
                 "RECORD should be rejected before pair verify completes");

    ok &= expect(allowed(rtsp_session_state::announced, rtsp_action::fairplay_setup),
                 "FairPlay setup should be allowed after pairing");
    ok &= expect(allowed(rtsp_session_state::announced, rtsp_action::announce),
                 "repeat ANNOUNCE should be allowed after pairing");
    ok &= expect(allowed(rtsp_session_state::announced, rtsp_action::setup),
                 "SETUP should be allowed after pairing");
    ok &= expect(!allowed(rtsp_session_state::announced, rtsp_action::record),
                 "RECORD should be rejected before SETUP");

    ok &= expect(allowed(rtsp_session_state::ready, rtsp_action::record),
                 "RECORD should be allowed after SETUP");
    ok &= expect(allowed(rtsp_session_state::ready, rtsp_action::flush),
                 "FLUSH should be allowed after SETUP");
    ok &= expect(!allowed(rtsp_session_state::ready, rtsp_action::pause),
                 "PAUSE should be rejected before playback");

    ok &= expect(allowed(rtsp_session_state::playing, rtsp_action::setup),
                 "stream SETUP should be allowed during playback");
    ok &= expect(allowed(rtsp_session_state::playing, rtsp_action::pause),
                 "PAUSE should be allowed during playback");
    ok &= expect(allowed(rtsp_session_state::playing, rtsp_action::control),
                 "control posts should be allowed during playback");
    ok &= expect(allowed(rtsp_session_state::paused, rtsp_action::record),
                 "RECORD should resume from paused state");
    ok &= expect(allowed(rtsp_session_state::teardown, rtsp_action::teardown),
                 "TEARDOWN should be allowed during teardown");
    ok &= expect(!allowed(rtsp_session_state::teardown, rtsp_action::control),
                 "control posts should be rejected after teardown");
    ok &= expect(!allowed(rtsp_session_state::teardown, rtsp_action::info),
                 "GET /info should be rejected after teardown");

    return ok ? 0 : 1;
}
