#pragma once

#include <cstdint>
#include <string_view>

namespace mirage::protocols::airplay {

enum class rtsp_session_state : uint8_t {
    init,
    pair_setup,
    pair_verify,
    announced,
    ready,
    playing,
    paused,
    teardown
};

enum class rtsp_action : uint8_t {
    options,
    info,
    pair_setup,
    pair_verify,
    fairplay_setup,
    announce,
    setup,
    record,
    pause,
    flush,
    teardown,
    get_parameter,
    set_parameter,
    control,
    unknown
};

[[nodiscard]] rtsp_action classify_rtsp_action(std::string_view method, std::string_view uri);
[[nodiscard]] bool rtsp_action_allowed(rtsp_session_state state, rtsp_action action);
[[nodiscard]] std::string_view rtsp_state_name(rtsp_session_state state);
[[nodiscard]] std::string_view rtsp_action_name(rtsp_action action);

}  // namespace mirage::protocols::airplay
