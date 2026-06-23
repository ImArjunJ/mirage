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
