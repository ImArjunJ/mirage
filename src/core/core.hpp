#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mirage {
enum class protocol : uint8_t { airplay, cast, miracast };
constexpr std::string_view to_string(protocol p) {
    switch (p) {
        case protocol::airplay:
            return "AirPlay";
        case protocol::cast:
            return "Cast";
        case protocol::miracast:
            return "Miracast";
    }
    std::unreachable();
}
enum class video_codec : uint8_t { h264, hevc, vp8, vp9, av1 };
constexpr std::string_view to_string(video_codec c) {
    switch (c) {
        case video_codec::h264:
            return "H.264";
        case video_codec::hevc:
            return "HEVC";
        case video_codec::vp8:
            return "VP8";
        case video_codec::vp9:
            return "VP9";
        case video_codec::av1:
            return "AV1";
    }
    std::unreachable();
}
enum class audio_codec : uint8_t { alac, aac, opus, pcm };
constexpr std::string_view to_string(audio_codec c) {
    switch (c) {
        case audio_codec::alac:
            return "ALAC";
        case audio_codec::aac:
            return "AAC";
        case audio_codec::opus:
            return "Opus";
        case audio_codec::pcm:
            return "PCM";
    }
    std::unreachable();
}
struct mirage_error {
    enum class kind : uint8_t {
        protocol_error,
        network,
        crypto,
        decode,
        render,
        config_error,
        discovery,
        pairing,
        session,
        timeout,
        internal,
    };
    kind error_kind;
    std::string message;
    std::source_location location;
    static mirage_error protocol_err(protocol p, std::string msg,
                                     std::source_location loc = std::source_location::current()) {
        return {kind::protocol_error, std::format("{}: {}", to_string(p), std::move(msg)), loc};
    }
    static mirage_error crypto(std::string msg,
                               std::source_location loc = std::source_location::current()) {
        return {kind::crypto, std::move(msg), loc};
    }
    static mirage_error network(std::string msg,
                                std::source_location loc = std::source_location::current()) {
        return {kind::network, std::move(msg), loc};
    }
    static mirage_error decode(std::string msg,
                               std::source_location loc = std::source_location::current()) {
        return {kind::decode, std::move(msg), loc};
    }
    static mirage_error render(std::string msg,
                               std::source_location loc = std::source_location::current()) {
        return {kind::render, std::move(msg), loc};
    }
    static mirage_error config_err(std::string msg,
                                   std::source_location loc = std::source_location::current()) {
        return {kind::config_error, std::move(msg), loc};
    }
    static mirage_error discovery(std::string msg,
                                  std::source_location loc = std::source_location::current()) {
        return {kind::discovery, std::move(msg), loc};
    }
    static mirage_error pairing(std::string msg,
                                std::source_location loc = std::source_location::current()) {
        return {kind::pairing, std::move(msg), loc};
    }
    static mirage_error session(std::string msg,
                                std::source_location loc = std::source_location::current()) {
        return {kind::session, std::move(msg), loc};
    }
    static mirage_error timeout(std::string msg,
                                std::source_location loc = std::source_location::current()) {
        return {kind::timeout, std::move(msg), loc};
    }
    static mirage_error internal(std::string msg,
                                 std::source_location loc = std::source_location::current()) {
        return {kind::internal, std::move(msg), loc};
    }
    static mirage_error cast_err(std::string msg,
                                 std::source_location loc = std::source_location::current()) {
        return {kind::protocol_error, std::format("Cast: {}", std::move(msg)), loc};
    }
    [[nodiscard]] std::string format() const {
        return std::format("[{}:{}] {}: {}", location.file_name(), location.line(),
                           kind_to_string(error_kind), message);
    }

private:
    static constexpr std::string_view kind_to_string(kind k) {
        switch (k) {
            case kind::protocol_error:
                return "Protocol";
            case kind::network:
                return "Network";
            case kind::crypto:
                return "Crypto";
            case kind::decode:
                return "Decode";
            case kind::render:
                return "Render";
            case kind::config_error:
                return "Config";
            case kind::discovery:
                return "Discovery";
            case kind::pairing:
                return "Pairing";
            case kind::session:
                return "Session";
            case kind::timeout:
                return "Timeout";
            case kind::internal:
                return "Internal";
        }
        std::unreachable();
    }
};
template <typename T>
using result = std::expected<T, mirage_error>;
template <typename T>
concept decryptor = requires(T& d, std::span<const std::byte> in, std::span<std::byte> out) {
    { d.decrypt(in, out) } -> std::same_as<result<size_t>>;
    { d.set_key(std::span<const std::byte>{}) } -> std::same_as<result<void>>;
};
template <typename T>
concept protocol_handler = requires(T& h) {
    { h.protocol() } -> std::same_as<protocol>;
    { h.start() } -> std::same_as<result<void>>;
    { h.stop() } -> std::same_as<void>;
};
template <typename T>
concept video_decoder = requires(T& d, std::span<const std::byte> nal) {
    typename T::frame;
    { d.decode(nal) } -> std::same_as<result<std::optional<typename T::frame>>>;
    { d.flush() } -> std::same_as<result<std::vector<typename T::frame>>>;
    { d.codec() } -> std::same_as<video_codec>;
};
template <typename T>
concept renderer = requires(T& r) {
    { r.present() } -> std::same_as<result<void>>;
    { r.resize(uint32_t{}, uint32_t{}) } -> std::same_as<result<void>>;
};
struct config {
    std::string device_name = "Mirage";
    uint16_t airplay_port = 7000;
    uint16_t cast_port = 8009;
    uint16_t miracast_port = 7236;
    bool enable_airplay = true;
    bool enable_cast = true;
    bool enable_miracast = true;
    bool hardware_decode = true;
    std::string log_level = "info";
    static result<config> load_from_file(std::string_view path);
    static config load_default() { return config{}; }
};
std::string base64_encode(std::span<const std::byte> data);
result<std::vector<std::byte>> base64_decode(std::string_view encoded);
std::string generate_uuid();
}  // namespace mirage
