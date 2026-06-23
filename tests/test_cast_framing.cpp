#include <cstddef>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protocols/cast/framing.hpp"
#include "protocols/cast/probe.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::vector<std::byte> bytes(std::string_view value) {
    auto raw = std::as_bytes(std::span<const char>(value.data(), value.size()));
    return {raw.begin(), raw.end()};
}

std::string text(const std::vector<std::byte>& value) {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

}  // namespace

int main() {
    bool ok = true;

    auto hello = bytes("hello");
    auto hello_frame = mirage::protocols::cast::make_channel_frame(hello);
    ok &= expect(hello_frame.has_value(), "hello frame build failed");

    mirage::protocols::cast::channel_frame_parser parser;
    auto appended_header = parser.append(std::span<const std::byte>(
        hello_frame->data(), mirage::protocols::cast::channel_frame_header_size));
    ok &= expect(appended_header.has_value(), "header append failed");
    ok &= expect(parser.pending_frames() == 0, "partial frame should not be ready");
    ok &= expect(parser.buffered_bytes() == mirage::protocols::cast::channel_frame_header_size,
                 "partial frame buffered byte mismatch");

    auto appended_payload = parser.append(std::span<const std::byte>(
        hello_frame->data() + mirage::protocols::cast::channel_frame_header_size, hello.size()));
    ok &= expect(appended_payload.has_value(), "payload append failed");
    auto parsed_hello = parser.next_frame();
    ok &= expect(parsed_hello.has_value(), "hello frame missing");
    ok &= expect(parsed_hello && text(*parsed_hello) == "hello", "hello payload mismatch");
    ok &= expect(!parser.next_frame().has_value(), "unexpected extra frame");

    auto one = mirage::protocols::cast::make_channel_frame(bytes("one"));
    auto two = mirage::protocols::cast::make_channel_frame(bytes("two"));
    ok &= expect(one.has_value() && two.has_value(), "coalesced frame build failed");
    std::vector<std::byte> coalesced;
    coalesced.insert(coalesced.end(), one->begin(), one->end());
    coalesced.insert(coalesced.end(), two->begin(), two->end());
    auto appended_coalesced = parser.append(coalesced);
    ok &= expect(appended_coalesced.has_value(), "coalesced append failed");
    auto parsed_one = parser.next_frame();
    auto parsed_two = parser.next_frame();
    ok &= expect(parsed_one && text(*parsed_one) == "one", "first coalesced payload mismatch");
    ok &= expect(parsed_two && text(*parsed_two) == "two", "second coalesced payload mismatch");

    auto oversized = bytes("payload");
    auto oversized_result = mirage::protocols::cast::make_channel_frame(oversized, 3);
    ok &= expect(!oversized_result.has_value(), "oversized payload should fail");

    std::vector<std::byte> invalid_header{
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x08},
    };
    mirage::protocols::cast::channel_frame_parser small_parser(4);
    auto invalid_append = small_parser.append(invalid_header);
    ok &= expect(!invalid_append.has_value(), "oversized frame header should fail");
    ok &= expect(small_parser.buffered_bytes() == 0, "invalid frame should clear buffer");

    const auto frame_view = std::string_view(
        reinterpret_cast<const char*>(hello_frame->data()), hello_frame->size());
    ok &= expect(mirage::protocols::cast::looks_like_channel_frame(frame_view),
                 "frame prefix was not detected");
    ok &= expect(!mirage::protocols::cast::looks_like_channel_frame("GET "),
                 "http prefix should not look like a frame");
    ok &= expect(!mirage::protocols::cast::looks_like_channel_frame("\x16\x03\x01", 3),
                 "short tls prefix should not look like a frame");

    auto probe = mirage::protocols::cast::handle_probe(frame_view, "Living Room");
    ok &= expect(probe.kind == mirage::protocols::cast::probe_kind::channel_frame,
                 "probe did not classify channel frame");
    ok &= expect(probe.response.empty(), "channel frame probe should not respond");

    return ok ? 0 : 1;
}
