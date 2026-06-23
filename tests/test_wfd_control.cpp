#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "protocols/wfd/control.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

mirage::protocols::rtsp_request_head request(std::string method, uint32_t cseq = 1) {
    return {
        .method = std::move(method),
        .uri = "rtsp://127.0.0.1/wfd1.0",
        .version = "RTSP/1.0",
        .headers = {{"CSeq", std::to_string(cseq)}},
        .cseq = cseq,
        .content_length = 0,
    };
}

}  // namespace

int main() {
    bool ok = true;

    auto all = mirage::protocols::wfd::capability_text();
    ok &= expect(contains(all, "wfd_audio_codecs:"), "audio capability missing");
    ok &= expect(contains(all, "wfd_video_formats:"), "video capability missing");
    ok &= expect(contains(all, "wfd_content_protection: none"),
                 "content protection capability missing");

    auto filtered = mirage::protocols::wfd::capability_text("wfd_audio_codecs\r\n");
    ok &= expect(contains(filtered, "wfd_audio_codecs:"), "filtered capability missing");
    ok &= expect(!contains(filtered, "wfd_video_formats:"),
                 "filtered capability included unexpected video formats");

    auto options = mirage::protocols::wfd::handle_control_request(request("OPTIONS"), "");
    ok &= expect(options.has_value(), "OPTIONS failed");
    if (options) {
        auto wire = mirage::protocols::wfd::serialize_control_response(*options, 7);
        ok &= expect(contains(wire, "RTSP/1.0 200 OK"), "OPTIONS status mismatch");
        ok &= expect(contains(wire, "CSeq: 7"), "OPTIONS cseq mismatch");
        ok &= expect(contains(wire, "Public: org.wfa.wfd1.0"), "OPTIONS public missing");
    }

    auto get = mirage::protocols::wfd::handle_control_request(request("GET_PARAMETER"),
                                                              "wfd_audio_codecs\r\n");
    ok &= expect(get.has_value(), "GET_PARAMETER failed");
    if (get) {
        ok &= expect(get->status_code == 200, "GET_PARAMETER status mismatch");
        ok &= expect(contains(get->body, "wfd_audio_codecs:"), "GET_PARAMETER body missing");
        ok &= expect(!contains(get->body, "wfd_video_formats:"),
                     "GET_PARAMETER body did not honor requested parameters");
    }

    auto accepted_set = mirage::protocols::wfd::analyze_set_parameters(
        "wfd_video_formats: 00 00 02 10 0001FFFF 1FFFFFFF 00000FFF 00 0000 0000 00 none "
        "none\r\n"
        "wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19000 0 mode=play\r\n");
    ok &= expect(accepted_set.result == mirage::protocols::wfd::set_parameter_result::accepted,
                 "accepted SET_PARAMETER analysis mismatch");

    auto trigger_set = mirage::protocols::wfd::analyze_set_parameters(
        "wfd_trigger_method: SETUP\r\n");
    ok &= expect(trigger_set.result ==
                     mirage::protocols::wfd::set_parameter_result::media_trigger,
                 "trigger SET_PARAMETER analysis mismatch");
    ok &= expect(trigger_set.parameter == "wfd_trigger_method",
                 "trigger parameter name mismatch");

    auto malformed_set = mirage::protocols::wfd::analyze_set_parameters("not-a-parameter\r\n");
    ok &= expect(malformed_set.result ==
                     mirage::protocols::wfd::set_parameter_result::unsupported_parameter,
                 "malformed SET_PARAMETER analysis mismatch");

    auto set = mirage::protocols::wfd::handle_control_request(
        request("SET_PARAMETER"), "wfd_client_rtp_ports: RTP/AVP/UDP;unicast 19000 0 mode=play\r\n");
    ok &= expect(set.has_value(), "SET_PARAMETER response missing");
    if (set) {
        ok &= expect(set->status_code == 200, "SET_PARAMETER status mismatch");
    }

    auto trigger = mirage::protocols::wfd::handle_control_request(
        request("SET_PARAMETER"), "wfd_trigger_method: SETUP\r\n");
    ok &= expect(trigger.has_value(), "trigger SET_PARAMETER response missing");
    if (trigger) {
        ok &= expect(trigger->status_code == 501,
                     "trigger SET_PARAMETER should be explicit unsupported media");
        ok &= expect(contains(trigger->body, "media-not-implemented"),
                     "trigger unsupported detail missing");
    }

    auto malformed = mirage::protocols::wfd::handle_control_request(
        request("SET_PARAMETER"), "not-a-parameter\r\n");
    ok &= expect(malformed.has_value(), "malformed SET_PARAMETER response missing");
    if (malformed) {
        ok &= expect(malformed->status_code == 451,
                     "malformed SET_PARAMETER status mismatch");
        ok &= expect(contains(malformed->body, "parameter-not-understood"),
                     "malformed SET_PARAMETER detail missing");
    }

    auto setup = mirage::protocols::wfd::handle_control_request(request("SETUP"), "");
    ok &= expect(setup.has_value(), "SETUP response missing");
    if (setup) {
        ok &= expect(setup->status_code == 501, "SETUP should be explicit unsupported media");
        ok &= expect(contains(setup->body, "media-not-implemented"),
                     "SETUP unsupported detail missing");
    }

    auto teardown = mirage::protocols::wfd::handle_control_request(request("TEARDOWN"), "");
    ok &= expect(teardown.has_value(), "TEARDOWN response missing");
    if (teardown) {
        ok &= expect(teardown->close_after_send, "TEARDOWN should close after send");
    }

    auto invalid_version = request("OPTIONS");
    invalid_version.version = "RTSP/2.0";
    auto invalid = mirage::protocols::wfd::handle_control_request(invalid_version, "");
    ok &= expect(!invalid.has_value(), "invalid RTSP version accepted");

    return ok ? 0 : 1;
}
