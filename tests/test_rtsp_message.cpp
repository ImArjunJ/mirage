#include <iostream>
#include <string>

#include "protocols/rtsp_message.hpp"

namespace {

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

    auto setup = parse_rtsp_request_head(
        "SETUP rtsp://127.0.0.1/stream RTSP/1.0\r\n"
        "cseq: 42\r\n"
        "Content-Length: 5\r\n"
        "Transport: RTP/AVP/UDP;unicast\r\n"
        "X-Whitespace:   padded value  \t\r\n"
        "Ignored-Header-Without-Colon\r\n"
        "\r\n");
    ok &= expect(setup.has_value(), "setup request did not parse");
    if (setup) {
        ok &= expect(setup->method == "SETUP", "method mismatch");
        ok &= expect(setup->uri == "rtsp://127.0.0.1/stream", "uri mismatch");
        ok &= expect(setup->version == "RTSP/1.0", "version mismatch");
        ok &= expect(setup->cseq.has_value() && *setup->cseq == 42, "cseq mismatch");
        ok &= expect(setup->content_length == 5, "content length mismatch");
        auto transport = find_rtsp_header(setup->headers, "transport");
        ok &= expect(transport.has_value() && *transport == "RTP/AVP/UDP;unicast",
                     "transport header mismatch");
        auto whitespace = find_rtsp_header(setup->headers, "x-whitespace");
        ok &= expect(whitespace.has_value() && *whitespace == "padded value",
                     "trimmed header mismatch");
    }

    auto no_body = parse_rtsp_request_head("OPTIONS * RTSP/1.0\r\nCSeq: 7\r\n\r\n");
    ok &= expect(no_body.has_value(), "options request did not parse");
    if (no_body) {
        ok &= expect(no_body->content_length == 0, "default content length mismatch");
        ok &= expect(no_body->cseq.has_value() && *no_body->cseq == 7, "options cseq mismatch");
    }

    ok &= expect(!parse_rtsp_request_head("BROKEN\r\n\r\n").has_value(),
                 "broken request line parsed");
    ok &= expect(!parse_rtsp_request_head(
                     "OPTIONS * RTSP/1.0\r\nContent-Length: nope\r\n\r\n")
                      .has_value(),
                 "invalid content length parsed");
    ok &= expect(!parse_rtsp_request_head("OPTIONS * RTSP/1.0\r\nCSeq: 4294967296\r\n\r\n")
                      .has_value(),
                 "oversized cseq parsed");
    ok &= expect(!parse_rtsp_request_head(
                     "OPTIONS * RTSP/1.0\r\nContent-Length: 67108865\r\n\r\n")
                      .has_value(),
                 "oversized content length parsed");

    return ok ? 0 : 1;
}
