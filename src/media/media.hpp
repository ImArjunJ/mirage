#pragma once
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/core.hpp"
namespace mirage::media {
struct decoded_frame {
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    std::vector<std::byte> y_plane;
    std::vector<std::byte> uv_plane;
    int stride_y = 0;
    int stride_uv = 0;
    decoded_frame() = default;
    decoded_frame(decoded_frame&&) = default;
    decoded_frame& operator=(decoded_frame&&) = default;
    ~decoded_frame() = default;
};
enum class nal_type : uint8_t {
    slice = 1,
    slice_a = 2,
    slice_b = 3,
    slice_c = 4,
    idr = 5,
    sei = 6,
    sps = 7,
    pps = 8,
    aud = 9,
    filler_data = 12,
    stap_a = 24,
    stap_b = 25,
    mtap16 = 26,
    mtap24 = 27,
    fu_a = 28,
    fu_b = 29,
};
struct nal_unit {
    nal_type type;
    std::vector<std::byte> data;
    [[nodiscard]] bool is_keyframe() const { return type == nal_type::idr; }
};
class nal_depacketizer {
public:
    nal_depacketizer() = default;
    result<std::vector<nal_unit>> process_rtp_payload(std::span<const std::byte> payload);

private:
    static result<std::vector<nal_unit>> process_stap_a(std::span<const std::byte> data);
    result<std::vector<nal_unit>> process_fu_a(std::span<const std::byte> data);
    std::vector<std::byte> fragment_buffer_;
    nal_type current_fragment_type_{};
};
struct rtp_header {
    uint8_t version = 2;
    bool padding = false;
    bool extension = false;
    uint8_t csrc_count = 0;
    bool marker = false;
    uint8_t payload_type = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    static result<rtp_header> parse(std::span<const std::byte> data);
    static constexpr size_t size = 12;
};
class vaapi_decoder {
public:
    using frame = decoded_frame;
    static result<vaapi_decoder> create(video_codec codec);
    result<std::optional<decoded_frame>> decode(std::span<const std::byte> nal);
    result<std::vector<decoded_frame>> flush();
    [[nodiscard]] video_codec codec() const { return codec_; }
    vaapi_decoder(vaapi_decoder&&) noexcept;
    vaapi_decoder& operator=(vaapi_decoder&&) noexcept;
    ~vaapi_decoder();

private:
    struct impl;
    explicit vaapi_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec);
    std::unique_ptr<impl> impl_;
    video_codec codec_;
};
class nvdec_decoder {
public:
    using frame = decoded_frame;
    static result<nvdec_decoder> create(video_codec codec);
    result<std::optional<decoded_frame>> decode(std::span<const std::byte> nal);
    result<std::vector<decoded_frame>> flush();
    [[nodiscard]] video_codec codec() const { return codec_; }
    nvdec_decoder(nvdec_decoder&&) noexcept;
    nvdec_decoder& operator=(nvdec_decoder&&) noexcept;
    ~nvdec_decoder();

private:
    struct impl;
    explicit nvdec_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec);
    std::unique_ptr<impl> impl_;
    video_codec codec_;
};
class ffmpeg_decoder {
public:
    using frame = decoded_frame;
    static result<ffmpeg_decoder> create(video_codec codec);
    result<std::optional<decoded_frame>> decode(std::span<const std::byte> nal);
    result<std::vector<decoded_frame>> flush();
    [[nodiscard]] video_codec codec() const { return codec_; }
    ffmpeg_decoder(ffmpeg_decoder&&) noexcept;
    ffmpeg_decoder& operator=(ffmpeg_decoder&&) noexcept;
    ~ffmpeg_decoder();

private:
    struct impl;
    explicit ffmpeg_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec);
    std::unique_ptr<impl> impl_;
    video_codec codec_;
};
void write_annex_b(std::ostream& out, const nal_unit& nal);
enum class decoder_backend : uint8_t { vaapi, nvdec, software };
class unified_decoder {
public:
    using frame = decoded_frame;
    static result<unified_decoder> create(video_codec codec, bool prefer_hardware = true);
    result<std::optional<decoded_frame>> decode(std::span<const std::byte> nal);
    result<std::vector<decoded_frame>> flush();
    [[nodiscard]] video_codec codec() const { return codec_; }
    [[nodiscard]] decoder_backend backend() const { return backend_; }
    [[nodiscard]] std::string_view backend_name() const;
    unified_decoder(unified_decoder&&) noexcept;
    unified_decoder& operator=(unified_decoder&&) noexcept;
    ~unified_decoder();

private:
    struct impl;
    explicit unified_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec,
                             decoder_backend backend);
    std::unique_ptr<impl> impl_;
    video_codec codec_;
    decoder_backend backend_;
};
}  // namespace mirage::media
