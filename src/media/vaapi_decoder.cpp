#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/core.hpp"
#include "media/media.hpp"
#ifdef __linux__
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
}
#endif
namespace mirage::media {
struct vaapi_decoder::impl {
#ifdef __linux__
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw_device_ctx = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* hw_frame = nullptr;
    AVFrame* sw_frame = nullptr;
    ~impl() {
        if (sw_frame) {
            av_frame_free(&sw_frame);
        }
        if (hw_frame) {
            av_frame_free(&hw_frame);
        }
        if (packet) {
            av_packet_free(&packet);
        }
        if (ctx) {
            avcodec_free_context(&ctx);
        }
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
    }
#endif
};
result<vaapi_decoder> vaapi_decoder::create(video_codec codec) {
#ifdef __linux__
    AVCodecID codec_id;
    switch (codec) {
        case video_codec::h264:
            codec_id = AV_CODEC_ID_H264;
            break;
        case video_codec::hevc:
            codec_id = AV_CODEC_ID_HEVC;
            break;
        default:
            return std::unexpected(
                mirage_error::decode("VA-API: unsupported codec for hardware decode"));
    }
    auto impl_ptr = std::make_unique<impl>();
    impl_ptr->codec = avcodec_find_decoder(codec_id);
    if (!impl_ptr->codec) {
        return std::unexpected(mirage_error::decode("VA-API: codec not found"));
    }
    impl_ptr->ctx = avcodec_alloc_context3(impl_ptr->codec);
    if (!impl_ptr->ctx) {
        return std::unexpected(mirage_error::decode("VA-API: failed to allocate context"));
    }
    const char* devices[] = {"/dev/dri/renderD128", "/dev/dri/renderD129", nullptr};
    bool hw_init = false;
    for (const char** dev = devices; *dev != nullptr; ++dev) {
        if (av_hwdevice_ctx_create(&impl_ptr->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, *dev, nullptr,
                                   0) == 0) {
            hw_init = true;
            break;
        }
    }
    if (!hw_init) {
        return std::unexpected(mirage_error::decode("VA-API: no hardware device available"));
    }
    impl_ptr->ctx->hw_device_ctx = av_buffer_ref(impl_ptr->hw_device_ctx);
    impl_ptr->ctx->get_format = [](AVCodecContext*, const AVPixelFormat* pix_fmts) {
        for (const AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_VAAPI) {
                return *p;
            }
        }
        return AV_PIX_FMT_NONE;
    };
    if (avcodec_open2(impl_ptr->ctx, impl_ptr->codec, nullptr) < 0) {
        return std::unexpected(mirage_error::decode("VA-API: failed to open codec"));
    }
    impl_ptr->packet = av_packet_alloc();
    impl_ptr->hw_frame = av_frame_alloc();
    impl_ptr->sw_frame = av_frame_alloc();
    if (!impl_ptr->packet || !impl_ptr->hw_frame || !impl_ptr->sw_frame) {
        return std::unexpected(mirage_error::decode("VA-API: failed to allocate frames"));
    }
    return vaapi_decoder{std::move(impl_ptr), codec};
#else
    (void)codec;
    return std::unexpected(mirage_error::decode("VA-API: not available on this platform"));
#endif
}
result<std::optional<decoded_frame>> vaapi_decoder::decode(std::span<const std::byte> nal) {
#ifdef __linux__
    impl_->packet->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(nal.data()));
    impl_->packet->size = static_cast<int>(nal.size());
    int ret = avcodec_send_packet(impl_->ctx, impl_->packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return std::unexpected(mirage_error::decode("VA-API: send packet failed"));
    }
    ret = avcodec_receive_frame(impl_->ctx, impl_->hw_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return std::nullopt;
    }
    if (ret < 0) {
        return std::unexpected(mirage_error::decode("VA-API: receive frame failed"));
    }
    ret = av_hwframe_transfer_data(impl_->sw_frame, impl_->hw_frame, 0);
    if (ret < 0) {
        av_frame_unref(impl_->hw_frame);
        return std::unexpected(mirage_error::decode("VA-API: GPU to CPU transfer failed"));
    }
    decoded_frame decoded;
    decoded.width = impl_->sw_frame->width;
    decoded.height = impl_->sw_frame->height;
    decoded.pts = impl_->sw_frame->pts;
    decoded.stride_y = impl_->sw_frame->linesize[0];
    decoded.y_plane.resize(static_cast<size_t>(decoded.stride_y) *
                           static_cast<size_t>(decoded.height));
    std::copy(impl_->sw_frame->data[0], impl_->sw_frame->data[0] + decoded.y_plane.size(),
              reinterpret_cast<uint8_t*>(decoded.y_plane.data()));
    if (impl_->sw_frame->data[1]) {
        decoded.stride_uv = impl_->sw_frame->linesize[1];
        decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv) *
                                static_cast<size_t>(decoded.height) / 2);
        std::copy(impl_->sw_frame->data[1], impl_->sw_frame->data[1] + decoded.uv_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
    }
    av_frame_unref(impl_->hw_frame);
    av_frame_unref(impl_->sw_frame);
    return decoded;
#else
    (void)nal;
    return std::unexpected(mirage_error::decode("VA-API: not available on this platform"));
#endif
}
result<std::vector<decoded_frame>> vaapi_decoder::flush() {
#ifdef __linux__
    avcodec_send_packet(impl_->ctx, nullptr);
    std::vector<decoded_frame> frames;
    while (true) {
        int ret = avcodec_receive_frame(impl_->ctx, impl_->hw_frame);
        if (ret < 0) {
            break;
        }
        ret = av_hwframe_transfer_data(impl_->sw_frame, impl_->hw_frame, 0);
        if (ret < 0) {
            av_frame_unref(impl_->hw_frame);
            continue;
        }
        decoded_frame decoded;
        decoded.width = impl_->sw_frame->width;
        decoded.height = impl_->sw_frame->height;
        decoded.pts = impl_->sw_frame->pts;
        decoded.stride_y = impl_->sw_frame->linesize[0];
        decoded.y_plane.resize(static_cast<size_t>(decoded.stride_y) *
                               static_cast<size_t>(decoded.height));
        std::copy(impl_->sw_frame->data[0], impl_->sw_frame->data[0] + decoded.y_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.y_plane.data()));
        if (impl_->sw_frame->data[1]) {
            decoded.stride_uv = impl_->sw_frame->linesize[1];
            decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv) *
                                    static_cast<size_t>(decoded.height) / 2);
            std::copy(impl_->sw_frame->data[1], impl_->sw_frame->data[1] + decoded.uv_plane.size(),
                      reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
        }
        frames.push_back(std::move(decoded));
        av_frame_unref(impl_->hw_frame);
        av_frame_unref(impl_->sw_frame);
    }
    return frames;
#else
    return std::unexpected(mirage_error::decode("VA-API: not available on this platform"));
#endif
}
vaapi_decoder::vaapi_decoder(std::unique_ptr<impl> impl_ptr, video_codec codec)
    : impl_(std::move(impl_ptr)), codec_(codec) {}
vaapi_decoder::vaapi_decoder(vaapi_decoder&&) noexcept = default;
vaapi_decoder& vaapi_decoder::operator=(vaapi_decoder&&) noexcept = default;
vaapi_decoder::~vaapi_decoder() = default;
}  // namespace mirage::media
