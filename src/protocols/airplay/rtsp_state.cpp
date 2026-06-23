#include "protocols/airplay/rtsp_state.hpp"

namespace mirage::protocols::airplay {

rtsp_action classify_rtsp_action(std::string_view method, std::string_view uri) {
    if (method == "OPTIONS") {
        return rtsp_action::options;
    }
    if (method == "GET" && uri == "/info") {
        return rtsp_action::info;
    }
    if (method == "POST" && uri == "/pair-setup") {
        return rtsp_action::pair_setup;
    }
    if (method == "POST" && uri == "/pair-verify") {
        return rtsp_action::pair_verify;
    }
    if (method == "POST" && uri == "/fp-setup") {
        return rtsp_action::fairplay_setup;
    }
    if (method == "ANNOUNCE") {
        return rtsp_action::announce;
    }
    if (method == "SETUP") {
        return rtsp_action::setup;
    }
    if (method == "RECORD") {
        return rtsp_action::record;
    }
    if (method == "PAUSE") {
        return rtsp_action::pause;
    }
    if (method == "FLUSH") {
        return rtsp_action::flush;
    }
    if (method == "TEARDOWN") {
        return rtsp_action::teardown;
    }
    if (method == "GET_PARAMETER") {
        return rtsp_action::get_parameter;
    }
    if (method == "SET_PARAMETER") {
        return rtsp_action::set_parameter;
    }
    if (method == "POST" && (uri == "/command" || uri == "/feedback" || uri == "/audioMode")) {
        return rtsp_action::control;
    }
    return rtsp_action::unknown;
}

bool rtsp_action_allowed(rtsp_session_state state, rtsp_action action) {
    if (state == rtsp_session_state::teardown) {
        return action == rtsp_action::teardown;
    }
    if (action == rtsp_action::unknown || action == rtsp_action::options ||
        action == rtsp_action::info || action == rtsp_action::teardown) {
        return true;
    }

    switch (action) {
        case rtsp_action::pair_setup:
            return state == rtsp_session_state::init || state == rtsp_session_state::pair_setup;
        case rtsp_action::pair_verify:
            return state == rtsp_session_state::init || state == rtsp_session_state::pair_setup ||
                   state == rtsp_session_state::pair_verify;
        case rtsp_action::fairplay_setup:
        case rtsp_action::get_parameter:
        case rtsp_action::set_parameter:
            return state == rtsp_session_state::announced || state == rtsp_session_state::ready ||
                   state == rtsp_session_state::playing || state == rtsp_session_state::paused;
        case rtsp_action::control:
            return true;
        case rtsp_action::announce:
            return state == rtsp_session_state::init || state == rtsp_session_state::announced ||
                   state == rtsp_session_state::ready || state == rtsp_session_state::playing ||
                   state == rtsp_session_state::paused;
        case rtsp_action::setup:
            return state == rtsp_session_state::announced || state == rtsp_session_state::ready ||
                   state == rtsp_session_state::playing || state == rtsp_session_state::paused;
        case rtsp_action::record:
            return state == rtsp_session_state::ready || state == rtsp_session_state::paused ||
                   state == rtsp_session_state::playing;
        case rtsp_action::pause:
            return state == rtsp_session_state::playing;
        case rtsp_action::flush:
            return state == rtsp_session_state::ready || state == rtsp_session_state::playing ||
                   state == rtsp_session_state::paused;
        case rtsp_action::options:
        case rtsp_action::info:
        case rtsp_action::teardown:
        case rtsp_action::unknown:
            return true;
    }

    return false;
}

std::string_view rtsp_state_name(rtsp_session_state state) {
    switch (state) {
        case rtsp_session_state::init:
            return "init";
        case rtsp_session_state::pair_setup:
            return "pair_setup";
        case rtsp_session_state::pair_verify:
            return "pair_verify";
        case rtsp_session_state::announced:
            return "announced";
        case rtsp_session_state::ready:
            return "ready";
        case rtsp_session_state::playing:
            return "playing";
        case rtsp_session_state::paused:
            return "paused";
        case rtsp_session_state::teardown:
            return "teardown";
    }

    return "unknown";
}

std::string_view rtsp_action_name(rtsp_action action) {
    switch (action) {
        case rtsp_action::options:
            return "options";
        case rtsp_action::info:
            return "info";
        case rtsp_action::pair_setup:
            return "pair_setup";
        case rtsp_action::pair_verify:
            return "pair_verify";
        case rtsp_action::fairplay_setup:
            return "fairplay_setup";
        case rtsp_action::announce:
            return "announce";
        case rtsp_action::setup:
            return "setup";
        case rtsp_action::record:
            return "record";
        case rtsp_action::pause:
            return "pause";
        case rtsp_action::flush:
            return "flush";
        case rtsp_action::teardown:
            return "teardown";
        case rtsp_action::get_parameter:
            return "get_parameter";
        case rtsp_action::set_parameter:
            return "set_parameter";
        case rtsp_action::control:
            return "control";
        case rtsp_action::unknown:
            return "unknown";
    }

    return "unknown";
}

}  // namespace mirage::protocols::airplay
