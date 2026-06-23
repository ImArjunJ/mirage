#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "protocols/airplay/media_metadata.hpp"

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

void append_be32(std::vector<std::byte>& out, size_t value) {
    out.push_back(std::byte{static_cast<uint8_t>((value >> 24) & 0xffU)});
    out.push_back(std::byte{static_cast<uint8_t>((value >> 16) & 0xffU)});
    out.push_back(std::byte{static_cast<uint8_t>((value >> 8) & 0xffU)});
    out.push_back(std::byte{static_cast<uint8_t>(value & 0xffU)});
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto c : text) {
        out.push_back(std::byte{static_cast<uint8_t>(c)});
    }
    return out;
}

std::vector<std::byte> atom(std::string_view tag, const std::vector<std::byte>& payload) {
    std::vector<std::byte> out;
    out.reserve(8 + payload.size());
    for (auto c : tag) {
        out.push_back(std::byte{static_cast<uint8_t>(c)});
    }
    append_be32(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void append(std::vector<std::byte>& out, std::vector<std::byte> value) {
    out.insert(out.end(), value.begin(), value.end());
}

}  // namespace

int main() {
    bool ok = true;

    std::vector<std::byte> item;
    append(item, atom("minm", bytes("song name")));
    append(item, atom("asar", bytes("artist name")));
    append(item, atom("asal", bytes("album name")));
    auto metadata_body = atom("mlit", item);
    auto metadata = mirage::airplay::parse_dmap_media_metadata(
        std::span<const std::byte>(metadata_body.data(), metadata_body.size()));

    ok &= expect(metadata.title == "song name", "title was not parsed");
    ok &= expect(metadata.artist == "artist name", "artist was not parsed");
    ok &= expect(metadata.album == "album name", "album was not parsed");
    ok &= expect(!metadata.empty(), "metadata should not be empty");

    auto progress =
        mirage::airplay::parse_airplay_progress("progress: 44100/88200/132300\r\n", 44100);
    ok &= expect(progress.has_value(), "progress was not parsed");
    if (progress) {
        ok &= expect(progress->position_ms == 1000, "position should be one second");
        ok &= expect(progress->duration_ms == 2000, "duration should be two seconds");
    }

    ok &= expect(!mirage::airplay::parse_airplay_progress("progress: 10/9/20", 44100),
                 "regressive progress should be rejected");
    ok &= expect(mirage::airplay::parse_dmap_media_metadata({}).empty(),
                 "empty metadata should stay empty");

    return ok ? 0 : 1;
}
