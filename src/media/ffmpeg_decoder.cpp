#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

#include "core/core.hpp"
#include "media/media.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
namespace mirage::media {
result<rtp_header> rtp_header::parse(std::span<const std::byte> data) {
    if (data.size() < size) {
        return std::unexpected(mirage_error::decode("RTP packet too short"));
    }
    rtp_header hdr;
    auto byte0 = static_cast<uint8_t>(data[0]);
    auto byte1 = static_cast<uint8_t>(data[1]);
    hdr.version = (byte0 >> 6) & 0x03;
    hdr.padding = ((byte0 >> 5) & 0x01) != 0;
    hdr.extension = ((byte0 >> 4) & 0x01) != 0;
    hdr.csrc_count = byte0 & 0x0F;
    hdr.marker = ((byte1 >> 7) & 0x01) != 0;
    hdr.payload_type = byte1 & 0x7F;
    hdr.sequence = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) |
                                         static_cast<uint16_t>(data[3]));
    hdr.timestamp = (static_cast<uint32_t>(data[4]) << 24) |
                    (static_cast<uint32_t>(data[5]) << 16) | (static_cast<uint32_t>(data[6]) << 8) |
                    static_cast<uint32_t>(data[7]);
    hdr.ssrc = (static_cast<uint32_t>(data[8]) << 24) | (static_cast<uint32_t>(data[9]) << 16) |
               (static_cast<uint32_t>(data[10]) << 8) | static_cast<uint32_t>(data[11]);
    return hdr;
}
result<std::vector<nal_unit>> nal_depacketizer::process_rtp_payload(
    std::span<const std::byte> payload) {
    if (payload.empty()) {
        return std::unexpected(mirage_error::decode("empty RTP payload"));
    }
    auto type = static_cast<nal_type>(static_cast<uint8_t>(payload[0]) & 0x1F);
    switch (type) {
        case nal_type::stap_a:
            return process_stap_a(payload.subspan(1));
        case nal_type::fu_a:
            return process_fu_a(payload);
        default:
            return std::vector{nal_unit{.type = type, .data = {payload.begin(), payload.end()}}};
    }
}
result<std::vector<nal_unit>> nal_depacketizer::process_stap_a(std::span<const std::byte> data) {
    std::vector<nal_unit> units;
    size_t offset = 0;
    while (offset + 2 < data.size()) {
        auto nal_size = static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                              static_cast<uint16_t>(data[offset + 1]));
        offset += 2;
        if (offset + nal_size > data.size()) {
            break;
        }
        auto nal_data = data.subspan(offset, nal_size);
        auto type = static_cast<nal_type>(static_cast<uint8_t>(nal_data[0]) & 0x1F);
        units.push_back({.type = type, .data = {nal_data.begin(), nal_data.end()}});
        offset += nal_size;
    }
    return units;
}
result<std::vector<nal_unit>> nal_depacketizer::process_fu_a(std::span<const std::byte> data) {
    if (data.size() < 2) {
        return std::unexpected(mirage_error::decode("FU-A packet too short"));
    }
    auto fu_header = static_cast<uint8_t>(data[1]);
    bool start = (fu_header & 0x80) != 0;
    bool end = (fu_header & 0x40) != 0;
    auto type = static_cast<nal_type>(fu_header & 0x1F);
    if (start) {
        fragment_buffer_.clear();
        auto nal_header = static_cast<uint8_t>((static_cast<uint8_t>(data[0]) & 0xE0) |
                                               static_cast<uint8_t>(type));
        fragment_buffer_.push_back(static_cast<std::byte>(nal_header));
        current_fragment_type_ = type;
    }
    fragment_buffer_.insert(fragment_buffer_.end(), data.begin() + 2, data.end());
    if (end) {
        return std::vector{
            nal_unit{.type = current_fragment_type_, .data = std::move(fragment_buffer_)}};
    }
    return std::vector<nal_unit>{};
}
void write_annex_b(std::ostream& out, const nal_unit& nal) {
    constexpr std::array<char, 4> start_code = {0x00, 0x00, 0x00, 0x01};
    out.write(start_code.data(), 4);
    out.write(reinterpret_cast<const char*>(nal.data.data()),
              static_cast<std::streamsize>(nal.data.size()));
}
struct ffmpeg_decoder::impl {
    const AVCodec* av_codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* av_frame = nullptr;
    ~impl() {
        if (av_frame) {
            av_frame_free(&av_frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};
result<ffmpeg_decoder> ffmpeg_decoder::create(video_codec codec) {
    AVCodecID codec_id;
    switch (codec) {
        case video_codec::h264:
            codec_id = AV_CODEC_ID_H264;
            break;
        case video_codec::hevc:
            codec_id = AV_CODEC_ID_HEVC;
            break;
        case video_codec::vp8:
            codec_id = AV_CODEC_ID_VP8;
            break;
        case video_codec::vp9:
            codec_id = AV_CODEC_ID_VP9;
            break;
        case video_codec::av1:
            codec_id = AV_CODEC_ID_AV1;
            break;
        default:
            return std::unexpected(mirage_error::decode("unsupported codec"));
    }
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->av_codec = avcodec_find_decoder(codec_id);
    if (!impl_ptr->av_codec) {
        return std::unexpected(
            mirage_error::decode(std::format("codec {} not found", to_string(codec))));
    }
    impl_ptr->ctx = avcodec_alloc_context3(impl_ptr->av_codec);
    if (!impl_ptr->ctx) {
        return std::unexpected(mirage_error::decode("failed to allocate codec context"));
    }
    if (avcodec_open2(impl_ptr->ctx, impl_ptr->av_codec, nullptr) < 0) {
        return std::unexpected(mirage_error::decode("failed to open codec"));
    }
    impl_ptr->packet = av_packet_alloc();
    impl_ptr->av_frame = av_frame_alloc();
    if (!impl_ptr->packet || !impl_ptr->av_frame) {
        return std::unexpected(mirage_error::decode("failed to allocate packet/frame"));
    }
    return ffmpeg_decoder{std::move(impl_ptr), codec};
}
static void convert_yuv420p_to_nv12_uv(const uint8_t* u_plane, int u_stride, const uint8_t* v_plane,
                                       int v_stride, std::vector<std::byte>& nv12_uv, int width,
                                       int height) {
    int uv_width = width / 2;
    int uv_height = height / 2;
    int nv12_stride = width;
    nv12_uv.resize(static_cast<size_t>(nv12_stride * uv_height));
    auto* dst = reinterpret_cast<uint8_t*>(nv12_uv.data());
    for (int y = 0; y < uv_height; ++y) {
        const uint8_t* u_row = u_plane + (y * u_stride);
        const uint8_t* v_row = v_plane + (y * v_stride);
        uint8_t* dst_row = dst + (y * nv12_stride);
        for (int x = 0; x < uv_width; ++x) {
            dst_row[x * 2] = u_row[x];
            dst_row[(x * 2) + 1] = v_row[x];
        }
    }
}
result<std::optional<decoded_frame>> ffmpeg_decoder::decode(std::span<const std::byte> nal) {
    impl_->packet->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(nal.data()));
    impl_->packet->size = static_cast<int>(nal.size());
    int ret = avcodec_send_packet(impl_->ctx, impl_->packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return std::unexpected(mirage_error::decode("failed to send packet to decoder"));
    }
    ret = avcodec_receive_frame(impl_->ctx, impl_->av_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return std::nullopt;
    }
    if (ret < 0) {
        return std::unexpected(mirage_error::decode("failed to receive frame from decoder"));
    }
    decoded_frame decoded;
    decoded.width = impl_->av_frame->width;
    decoded.height = impl_->av_frame->height;
    decoded.pts = impl_->av_frame->pts;
    decoded.stride_y = impl_->av_frame->linesize[0];
    decoded.y_plane.resize(static_cast<size_t>(decoded.stride_y * decoded.height));
    std::copy(impl_->av_frame->data[0], impl_->av_frame->data[0] + decoded.y_plane.size(),
              reinterpret_cast<uint8_t*>(decoded.y_plane.data()));
    auto pix_fmt = static_cast<AVPixelFormat>(impl_->av_frame->format);
    if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
        decoded.stride_uv = impl_->av_frame->linesize[1];
        int uv_height = decoded.height / 2;
        decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv * uv_height));
        std::copy(impl_->av_frame->data[1], impl_->av_frame->data[1] + decoded.uv_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
    } else if (pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P) {
        decoded.stride_uv = decoded.width;
        convert_yuv420p_to_nv12_uv(impl_->av_frame->data[1], impl_->av_frame->linesize[1],
                                   impl_->av_frame->data[2], impl_->av_frame->linesize[2],
                                   decoded.uv_plane, decoded.width, decoded.height);
    } else if (impl_->av_frame->data[1]) {
        decoded.stride_uv = impl_->av_frame->linesize[1];
        int uv_height = decoded.height / 2;
        decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv * uv_height));
        std::copy(impl_->av_frame->data[1], impl_->av_frame->data[1] + decoded.uv_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
    }
    av_frame_unref(impl_->av_frame);
    return decoded;
}
result<std::vector<decoded_frame>> ffmpeg_decoder::flush() {
    avcodec_send_packet(impl_->ctx, nullptr);
    std::vector<decoded_frame> frames;
    while (true) {
        int ret = avcodec_receive_frame(impl_->ctx, impl_->av_frame);
        if (ret < 0) {
            break;
        }
        decoded_frame decoded;
        decoded.width = impl_->av_frame->width;
        decoded.height = impl_->av_frame->height;
        decoded.pts = impl_->av_frame->pts;
        decoded.stride_y = impl_->av_frame->linesize[0];
        decoded.y_plane.resize(static_cast<size_t>(decoded.stride_y * decoded.height));
        std::copy(impl_->av_frame->data[0], impl_->av_frame->data[0] + decoded.y_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.y_plane.data()));
        auto pix_fmt = static_cast<AVPixelFormat>(impl_->av_frame->format);
        if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
            decoded.stride_uv = impl_->av_frame->linesize[1];
            decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv * decoded.height / 2));
            std::copy(impl_->av_frame->data[1], impl_->av_frame->data[1] + decoded.uv_plane.size(),
                      reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
        } else if (pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P) {
            decoded.stride_uv = decoded.width;
            convert_yuv420p_to_nv12_uv(impl_->av_frame->data[1], impl_->av_frame->linesize[1],
                                       impl_->av_frame->data[2], impl_->av_frame->linesize[2],
                                       decoded.uv_plane, decoded.width, decoded.height);
        } else if (impl_->av_frame->data[1]) {
            decoded.stride_uv = impl_->av_frame->linesize[1];
            decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv * decoded.height / 2));
            std::copy(impl_->av_frame->data[1], impl_->av_frame->data[1] + decoded.uv_plane.size(),
                      reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
        }
        frames.push_back(std::move(decoded));
        av_frame_unref(impl_->av_frame);
    }
    return frames;
}
ffmpeg_decoder::ffmpeg_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec)
    : impl_(std::move(impl_ptr)), codec_(codec) {}
ffmpeg_decoder::ffmpeg_decoder(ffmpeg_decoder&&) noexcept = default;
ffmpeg_decoder& ffmpeg_decoder::operator=(ffmpeg_decoder&&) noexcept = default;
ffmpeg_decoder::~ffmpeg_decoder() = default;
struct unified_decoder::impl {
    std::optional<vaapi_decoder> vaapi;
    std::optional<nvdec_decoder> nvdec;
    std::optional<ffmpeg_decoder> ffmpeg;
};
result<unified_decoder> unified_decoder::create(video_codec codec, bool prefer_hardware) {
    auto impl_ptr = std::make_unique<impl>();
    if (prefer_hardware) {
        auto vaapi_result = vaapi_decoder::create(codec);
        if (vaapi_result) {
            impl_ptr->vaapi = std::move(*vaapi_result);
            return unified_decoder{std::move(impl_ptr), codec, decoder_backend::vaapi};
        }
        auto nvdec_result = nvdec_decoder::create(codec);
        if (nvdec_result) {
            impl_ptr->nvdec = std::move(*nvdec_result);
            return unified_decoder{std::move(impl_ptr), codec, decoder_backend::nvdec};
        }
    }
    auto ffmpeg_result = ffmpeg_decoder::create(codec);
    if (!ffmpeg_result) {
        return std::unexpected(ffmpeg_result.error());
    }
    impl_ptr->ffmpeg = std::move(*ffmpeg_result);
    return unified_decoder{std::move(impl_ptr), codec, decoder_backend::software};
}
result<std::optional<decoded_frame>> unified_decoder::decode(std::span<const std::byte> nal) {
    if (impl_->vaapi) {
        return impl_->vaapi->decode(nal);
    }
    if (impl_->nvdec) {
        return impl_->nvdec->decode(nal);
    }
    if (impl_->ffmpeg) {
        return impl_->ffmpeg->decode(nal);
    }
    return std::unexpected(mirage_error::decode("no decoder initialized"));
}
result<std::vector<decoded_frame>> unified_decoder::flush() {
    if (impl_->vaapi) {
        return impl_->vaapi->flush();
    }
    if (impl_->nvdec) {
        return impl_->nvdec->flush();
    }
    if (impl_->ffmpeg) {
        return impl_->ffmpeg->flush();
    }
    return std::unexpected(mirage_error::decode("no decoder initialized"));
}
std::string_view unified_decoder::backend_name() const {
    switch (backend_) {
        case decoder_backend::vaapi:
            return "VA-API";
        case decoder_backend::nvdec:
            return "NVDEC";
        case decoder_backend::software:
            return "FFmpeg (software)";
    }
    return "unknown";
}
unified_decoder::unified_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec,
                                 decoder_backend backend)
    : impl_(std::move(impl_ptr)), codec_(codec), backend_(backend) {}
unified_decoder::unified_decoder(unified_decoder&&) noexcept = default;
unified_decoder& unified_decoder::operator=(unified_decoder&&) noexcept = default;
unified_decoder::~unified_decoder() = default;
}  // namespace mirage::media
