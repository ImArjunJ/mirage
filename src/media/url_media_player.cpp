#include "media/url_media_player.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/audio.hpp"
#include "core/log.hpp"
#include "media/media.hpp"
#include "render/render.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
}

namespace mirage::media {
namespace {

std::string av_error_string(int err) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buf{};
    if (av_strerror(err, buf.data(), buf.size()) < 0) {
        return std::to_string(err);
    }
    return std::string(buf.data());
}

void convert_yuv420p_to_nv12_uv(const uint8_t* u_plane, int u_stride, const uint8_t* v_plane,
                                int v_stride, std::vector<std::byte>& nv12_uv, int width,
                                int height) {
    const int uv_width = width / 2;
    const int uv_height = height / 2;
    const int nv12_stride = width;
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

result<decoded_frame> copy_video_frame(const AVFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.linesize[0] <= 0 ||
        frame.data[0] == nullptr) {
        return std::unexpected(mirage_error::decode("invalid video frame"));
    }

    decoded_frame decoded;
    decoded.width = frame.width;
    decoded.height = frame.height;
    decoded.pts = frame.pts;
    decoded.stride_y = frame.linesize[0];
    decoded.y_plane.resize(static_cast<size_t>(decoded.stride_y * decoded.height));
    std::copy(frame.data[0], frame.data[0] + decoded.y_plane.size(),
              reinterpret_cast<uint8_t*>(decoded.y_plane.data()));

    const auto pix_fmt = static_cast<AVPixelFormat>(frame.format);
    if ((pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) && frame.data[1] != nullptr &&
        frame.linesize[1] > 0) {
        decoded.stride_uv = frame.linesize[1];
        const int uv_height = decoded.height / 2;
        decoded.uv_plane.resize(static_cast<size_t>(decoded.stride_uv * uv_height));
        std::copy(frame.data[1], frame.data[1] + decoded.uv_plane.size(),
                  reinterpret_cast<uint8_t*>(decoded.uv_plane.data()));
        return decoded;
    }

    if ((pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P) &&
        frame.data[1] != nullptr && frame.data[2] != nullptr && frame.linesize[1] > 0 &&
        frame.linesize[2] > 0) {
        decoded.stride_uv = decoded.width;
        convert_yuv420p_to_nv12_uv(frame.data[1], frame.linesize[1], frame.data[2],
                                   frame.linesize[2], decoded.uv_plane, decoded.width,
                                   decoded.height);
        return decoded;
    }

    return std::unexpected(mirage_error::decode("unsupported video pixel format"));
}

struct codec_context_deleter {
    void operator()(AVCodecContext* ctx) const {
        if (ctx != nullptr) {
            avcodec_free_context(&ctx);
        }
    }
};

struct format_context_deleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx != nullptr) {
            avformat_close_input(&ctx);
        }
    }
};

struct frame_deleter {
    void operator()(AVFrame* frame) const {
        if (frame != nullptr) {
            av_frame_free(&frame);
        }
    }
};

struct packet_deleter {
    void operator()(AVPacket* packet) const {
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
    }
};

struct swr_deleter {
    void operator()(SwrContext* swr) const {
        if (swr != nullptr) {
            swr_free(&swr);
        }
    }
};

using codec_context_ptr = std::unique_ptr<AVCodecContext, codec_context_deleter>;
using format_context_ptr = std::unique_ptr<AVFormatContext, format_context_deleter>;
using frame_ptr = std::unique_ptr<AVFrame, frame_deleter>;
using packet_ptr = std::unique_ptr<AVPacket, packet_deleter>;
using swr_ptr = std::unique_ptr<SwrContext, swr_deleter>;

codec_context_ptr open_decoder(const AVStream& stream) {
    const auto* codec = avcodec_find_decoder(stream.codecpar->codec_id);
    if (codec == nullptr) {
        log::warn("Cast media decoder not found for codec id {}",
                  static_cast<int>(stream.codecpar->codec_id));
        return {};
    }

    codec_context_ptr ctx(avcodec_alloc_context3(codec));
    if (!ctx) {
        log::warn("failed to allocate Cast media decoder context");
        return {};
    }

    int ret = avcodec_parameters_to_context(ctx.get(), stream.codecpar);
    if (ret < 0) {
        log::warn("failed to copy Cast media codec parameters: {}", av_error_string(ret));
        return {};
    }
    if (ctx->ch_layout.nb_channels == 0 && stream.codecpar->ch_layout.nb_channels > 0) {
        av_channel_layout_copy(&ctx->ch_layout, &stream.codecpar->ch_layout);
    }
    if (ctx->ch_layout.nb_channels == 0) {
        av_channel_layout_default(&ctx->ch_layout, 2);
    }

    ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0) {
        log::warn("failed to open Cast media decoder: {}", av_error_string(ret));
        return {};
    }

    return ctx;
}

swr_ptr create_audio_resampler(const AVCodecContext& ctx, int& out_sample_rate, int& out_channels) {
    out_sample_rate = ctx.sample_rate > 0 ? ctx.sample_rate : 44100;
    out_channels = 2;

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, out_channels);
    SwrContext* raw = nullptr;
    int ret = swr_alloc_set_opts2(&raw, &out_layout, AV_SAMPLE_FMT_S16, out_sample_rate,
                                  &ctx.ch_layout, ctx.sample_fmt, ctx.sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || raw == nullptr) {
        log::warn("failed to allocate Cast media audio resampler");
        return {};
    }

    swr_ptr swr(raw);
    ret = swr_init(swr.get());
    if (ret < 0) {
        log::warn("failed to initialize Cast media audio resampler: {}", av_error_string(ret));
        return {};
    }
    return swr;
}

int interrupt_callback(void* opaque) {
    auto* stop_requested = static_cast<std::atomic_bool*>(opaque);
    return stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed) ? 1 : 0;
}

}  // namespace

struct url_media_player::impl {
    std::thread worker;
    std::atomic_bool stop_requested{false};
    std::atomic_bool paused{false};

    mutable std::mutex status_mutex;
    remote_media_playback_status playback_status;
    std::mutex callback_mutex;
    remote_media_status_callback status_callback;

    std::mutex command_mutex;
    bool seek_requested = false;
    double seek_seconds = 0.0;
    double playback_rate = 1.0;

    std::mutex player_mutex;
    audio::audio_player* audio_player = nullptr;
    float volume_db = 0.0F;
    float linear_volume = 1.0F;

    void emit_status(remote_media_playback_status status) {
        remote_media_status_callback callback;
        {
            std::scoped_lock lock(callback_mutex);
            callback = status_callback;
        }
        if (callback) {
            callback(std::move(status));
        }
    }

    void update_status(remote_media_playback_status status) {
        remote_media_playback_status snapshot;
        {
            std::scoped_lock lock(status_mutex);
            playback_status = std::move(status);
            snapshot = playback_status;
        }
        emit_status(std::move(snapshot));
    }

    void update_detail(std::string detail) {
        remote_media_playback_status snapshot;
        {
            std::scoped_lock lock(status_mutex);
            playback_status.detail = std::move(detail);
            snapshot = playback_status;
        }
        emit_status(std::move(snapshot));
    }

    void update_counts(uint64_t decoded_audio_frames, uint64_t decoded_video_frames) {
        remote_media_playback_status snapshot;
        {
            std::scoped_lock lock(status_mutex);
            playback_status.decoded_audio_frames = decoded_audio_frames;
            playback_status.decoded_video_frames = decoded_video_frames;
            snapshot = playback_status;
        }
        emit_status(std::move(snapshot));
    }

    void mark_stopped() {
        remote_media_playback_status snapshot;
        {
            std::scoped_lock lock(status_mutex);
            playback_status.active = false;
            playback_status.detail = "stopped";
            snapshot = playback_status;
        }
        emit_status(std::move(snapshot));
    }

    void set_status_callback(remote_media_status_callback callback) {
        std::scoped_lock lock(callback_mutex);
        status_callback = std::move(callback);
    }

    void set_paused(bool value) {
        paused.store(value, std::memory_order_relaxed);
        update_detail(value ? "paused" : "playing");
    }

    void request_seek(double seconds) {
        {
            std::scoped_lock lock(command_mutex);
            seek_requested = true;
            seek_seconds = std::max(0.0, seconds);
        }
        update_detail("seeking");
    }

    std::optional<double> take_seek_request() {
        std::scoped_lock lock(command_mutex);
        if (!seek_requested) {
            return std::nullopt;
        }
        seek_requested = false;
        return seek_seconds;
    }

    void set_rate(double rate) {
        {
            std::scoped_lock lock(command_mutex);
            playback_rate = std::max(0.0, rate);
        }
        update_detail("playback_rate");
    }

    void set_audio_player(audio::audio_player* player) {
        std::scoped_lock lock(player_mutex);
        audio_player = player;
        if (audio_player != nullptr) {
            audio_player->set_volume(linear_volume);
        }
    }

    void apply_volume(float db, float linear) {
        std::scoped_lock lock(player_mutex);
        volume_db = db;
        linear_volume = std::clamp(linear, 0.0F, 1.0F);
        if (audio_player != nullptr) {
            audio_player->set_volume(linear_volume);
        }
    }

    void run(remote_media_load request);
};

namespace {

uint64_t decode_audio_packet(AVCodecContext& ctx, SwrContext& swr, AVFrame& frame,
                             const AVPacket& packet, audio::audio_player* player,
                             int out_channels) {
    int ret = avcodec_send_packet(&ctx, &packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        log::warn("Cast media audio packet decode failed: {}", av_error_string(ret));
        return 0;
    }

    uint64_t decoded_frames = 0;
    while (true) {
        ret = avcodec_receive_frame(&ctx, &frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            log::warn("Cast media audio frame decode failed: {}", av_error_string(ret));
            break;
        }

        int max_out_samples = swr_get_out_samples(&swr, frame.nb_samples);
        if (max_out_samples < 0) {
            max_out_samples = frame.nb_samples;
        }

        const size_t bytes_per_sample = static_cast<size_t>(out_channels) * sizeof(int16_t);
        std::vector<uint8_t> pcm(static_cast<size_t>(max_out_samples) * bytes_per_sample);
        uint8_t* out_buf = pcm.data();
        const int converted =
            swr_convert(&swr, &out_buf, max_out_samples,
                        const_cast<const uint8_t**>(frame.extended_data), frame.nb_samples);
        if (converted < 0) {
            log::warn("Cast media audio resample failed: {}", av_error_string(converted));
            av_frame_unref(&frame);
            continue;
        }

        const size_t out_bytes = static_cast<size_t>(converted) * bytes_per_sample;
        if (player != nullptr) {
            player->push_pcm(std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(pcm.data()), out_bytes));
        }
        ++decoded_frames;
        av_frame_unref(&frame);
    }
    return decoded_frames;
}

uint64_t decode_video_packet(AVCodecContext& ctx, AVFrame& frame, const AVPacket& packet,
                             const remote_media_load& request,
                             std::unique_ptr<render::render_window>& renderer) {
    int ret = avcodec_send_packet(&ctx, &packet);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        log::warn("Cast media video packet decode failed: {}", av_error_string(ret));
        return 0;
    }

    uint64_t decoded_frames = 0;
    while (true) {
        ret = avcodec_receive_frame(&ctx, &frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            log::warn("Cast media video frame decode failed: {}", av_error_string(ret));
            break;
        }

        auto decoded = copy_video_frame(frame);
        av_frame_unref(&frame);
        if (!decoded) {
            log::debug("Cast media video frame skipped: {}", decoded.error().message);
            continue;
        }

        if (!renderer) {
            const auto title = request.title.empty() ? std::string("Mirage") : request.title;
            renderer = render::render_window::create(title, static_cast<uint32_t>(decoded->width),
                                                     static_cast<uint32_t>(decoded->height));
        }
        if (renderer && renderer->is_open()) {
            renderer->submit_frame(std::move(*decoded));
        }
        ++decoded_frames;
    }
    return decoded_frames;
}

}  // namespace

void url_media_player::impl::run(remote_media_load request) {
    avformat_network_init();
    paused.store(!request.autoplay, std::memory_order_relaxed);
    {
        std::scoped_lock lock(command_mutex);
        seek_requested = false;
        seek_seconds = std::max(0.0, request.start_time);
        playback_rate = 1.0;
    }
    update_status({
        .active = true,
        .audio = false,
        .video = false,
        .audio_output = false,
        .decoded_audio_frames = 0,
        .decoded_video_frames = 0,
        .detail = "opening",
    });

    AVFormatContext* raw_format = avformat_alloc_context();
    if (raw_format == nullptr) {
        update_status({.active = false, .detail = "open_failed"});
        log::warn("Cast media open failed for {}: could not allocate format context", request.url);
        return;
    }
    raw_format->interrupt_callback.callback = interrupt_callback;
    raw_format->interrupt_callback.opaque = &stop_requested;

    int ret = avformat_open_input(&raw_format, request.url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        if (raw_format != nullptr) {
            avformat_close_input(&raw_format);
        }
        update_status({.active = false, .detail = "open_failed"});
        log::warn("Cast media open failed for {}: {}", request.url, av_error_string(ret));
        return;
    }
    format_context_ptr format(raw_format);

    ret = avformat_find_stream_info(format.get(), nullptr);
    if (ret < 0) {
        update_status({.active = false, .detail = "stream_info_failed"});
        log::warn("Cast media stream info failed for {}: {}", request.url, av_error_string(ret));
        return;
    }

    const int audio_stream =
        av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    const int video_stream =
        av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    codec_context_ptr audio_ctx;
    swr_ptr audio_swr;
    std::unique_ptr<audio::audio_player> audio_output;
    int audio_rate = 44100;
    int audio_channels = 2;
    if (audio_stream >= 0) {
        audio_ctx = open_decoder(*format->streams[audio_stream]);
        if (audio_ctx) {
            audio_swr = create_audio_resampler(*audio_ctx, audio_rate, audio_channels);
        }
        if (audio_swr) {
            audio_output = audio::audio_player::create(audio_rate, audio_channels);
            if (audio_output) {
                set_audio_player(audio_output.get());
            } else {
                log::warn("Cast media audio output unavailable; decoded PCM will be discarded");
            }
        }
    }

    codec_context_ptr video_ctx;
    std::unique_ptr<render::render_window> renderer;
    if (video_stream >= 0) {
        video_ctx = open_decoder(*format->streams[video_stream]);
    }

    const bool audio_ready = audio_ctx != nullptr && audio_swr != nullptr;
    const bool video_ready = video_ctx != nullptr;
    if (!audio_ready && !video_ready) {
        update_status({.active = false, .detail = "no_supported_streams"});
        log::warn("Cast media has no supported audio or video streams: {}", request.url);
        return;
    }

    update_status({
        .active = true,
        .audio = audio_ready,
        .video = video_ready,
        .audio_output = audio_output != nullptr,
        .decoded_audio_frames = 0,
        .decoded_video_frames = 0,
        .detail = request.autoplay ? "playing" : "paused",
    });
    log::diagnostic("Cast media renderer started: url={}, audio={}, video={}, audio_output={}",
                    request.url, audio_ready ? "true" : "false", video_ready ? "true" : "false",
                    audio_output ? "true" : "false");

    if (request.start_time > 0.0) {
        const auto timestamp = static_cast<int64_t>(request.start_time * AV_TIME_BASE);
        av_seek_frame(format.get(), -1, timestamp, AVSEEK_FLAG_BACKWARD);
    }

    packet_ptr packet(av_packet_alloc());
    frame_ptr audio_frame(av_frame_alloc());
    frame_ptr video_frame(av_frame_alloc());
    if (!packet || !audio_frame || !video_frame) {
        update_status({.active = false, .detail = "allocation_failed"});
        set_audio_player(nullptr);
        log::warn("Cast media renderer allocation failed");
        return;
    }

    uint64_t decoded_audio_frames = 0;
    uint64_t decoded_video_frames = 0;
    while (!stop_requested.load(std::memory_order_relaxed)) {
        if (auto seek = take_seek_request()) {
            const auto timestamp = static_cast<int64_t>(*seek * AV_TIME_BASE);
            ret = av_seek_frame(format.get(), -1, timestamp, AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                log::warn("Cast media seek failed for {}: {}", request.url, av_error_string(ret));
            } else {
                if (audio_ctx) {
                    avcodec_flush_buffers(audio_ctx.get());
                }
                if (video_ctx) {
                    avcodec_flush_buffers(video_ctx.get());
                }
                update_status({
                    .active = true,
                    .audio = audio_ready,
                    .video = video_ready,
                    .audio_output = audio_output != nullptr,
                    .decoded_audio_frames = decoded_audio_frames,
                    .decoded_video_frames = decoded_video_frames,
                    .detail = paused.load(std::memory_order_relaxed) ? "paused" : "playing",
                });
                log::diagnostic("Cast media renderer seek: url={}, position={}", request.url,
                                *seek);
            }
        }
        if (paused.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        ret = av_read_frame(format.get(), packet.get());
        if (ret < 0) {
            break;
        }

        if (packet->stream_index == audio_stream && audio_ready) {
            decoded_audio_frames += decode_audio_packet(
                *audio_ctx, *audio_swr, *audio_frame, *packet, audio_output.get(), audio_channels);
        } else if (packet->stream_index == video_stream && video_ready) {
            decoded_video_frames +=
                decode_video_packet(*video_ctx, *video_frame, *packet, request, renderer);
        }
        av_packet_unref(packet.get());
    }

    set_audio_player(nullptr);
    if (stop_requested.load(std::memory_order_relaxed)) {
        update_status({
            .active = false,
            .audio = audio_ready,
            .video = video_ready,
            .audio_output = audio_output != nullptr,
            .decoded_audio_frames = decoded_audio_frames,
            .decoded_video_frames = decoded_video_frames,
            .detail = "stopped",
        });
    } else {
        update_status({
            .active = false,
            .audio = audio_ready,
            .video = video_ready,
            .audio_output = audio_output != nullptr,
            .decoded_audio_frames = decoded_audio_frames,
            .decoded_video_frames = decoded_video_frames,
            .detail = "finished",
        });
    }
    update_counts(decoded_audio_frames, decoded_video_frames);
    log::diagnostic(
        "Cast media renderer stopped: url={}, decoded_audio_frames={}, decoded_video_frames={}",
        request.url, decoded_audio_frames, decoded_video_frames);
}

url_media_player::url_media_player() : impl_(std::make_unique<impl>()) {}

url_media_player::~url_media_player() {
    stop();
}

void url_media_player::load(remote_media_load request) {
    stop();
    if (request.url.empty()) {
        impl_->update_status({.active = false, .detail = "empty_url"});
        return;
    }

    impl_->stop_requested.store(false, std::memory_order_relaxed);
    impl_->worker =
        std::thread([impl = impl_.get(), request = std::move(request)] { impl->run(request); });
}

void url_media_player::stop() {
    impl_->stop_requested.store(true, std::memory_order_relaxed);
    impl_->paused.store(false, std::memory_order_relaxed);
    impl_->set_audio_player(nullptr);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->mark_stopped();
}

void url_media_player::play() {
    impl_->set_paused(false);
}

void url_media_player::pause() {
    impl_->set_paused(true);
}

void url_media_player::seek(double seconds) {
    impl_->request_seek(seconds);
}

void url_media_player::set_playback_rate(double rate) {
    impl_->set_rate(rate);
}

void url_media_player::set_volume(float db, float linear) {
    impl_->apply_volume(db, linear);
}

void url_media_player::set_status_callback(remote_media_status_callback callback) {
    impl_->set_status_callback(std::move(callback));
}

remote_media_playback_status url_media_player::status() const {
    std::scoped_lock lock(impl_->status_mutex);
    return impl_->playback_status;
}

}  // namespace mirage::media
