#include <cstring>

#include "audio/audio.hpp"
#include "core/log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace mirage::audio {

struct audio_decoder::impl {
    const AVCodec* codec = nullptr;
    AVCodecContext* ctx = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int out_sample_rate = 44100;
    int out_channels = 2;

    ~impl() {
        if (frame)
            av_frame_free(&frame);
        if (packet)
            av_packet_free(&packet);
        if (swr)
            swr_free(&swr);
        if (ctx)
            avcodec_free_context(&ctx);
    }
};

audio_decoder::~audio_decoder() = default;

std::unique_ptr<audio_decoder> audio_decoder::create_aac(int sample_rate, int channels) {
    auto decoder = std::unique_ptr<audio_decoder>(new audio_decoder());
    decoder->impl_ = std::make_unique<impl>();
    auto& p = *decoder->impl_;

    p.codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!p.codec) {
        log::error("AAC decoder not found");
        return nullptr;
    }

    p.ctx = avcodec_alloc_context3(p.codec);
    if (!p.ctx) {
        log::error("failed to allocate AAC codec context");
        return nullptr;
    }

    p.ctx->sample_rate = sample_rate;
    av_channel_layout_default(&p.ctx->ch_layout, channels);

    static constexpr uint8_t aac_eld_config[] = {0xF8, 0xE8, 0x50, 0x00};
    p.ctx->extradata =
        static_cast<uint8_t*>(av_mallocz(sizeof(aac_eld_config) + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!p.ctx->extradata) {
        log::error("failed to allocate AAC extradata");
        return nullptr;
    }
    std::memcpy(p.ctx->extradata, aac_eld_config, sizeof(aac_eld_config));
    p.ctx->extradata_size = static_cast<int>(sizeof(aac_eld_config));

    int ret = avcodec_open2(p.ctx, p.codec, nullptr);
    if (ret < 0) {
        log::error("failed to open AAC decoder: {}", ret);
        return nullptr;
    }

    ret = swr_alloc_set_opts2(&p.swr, &p.ctx->ch_layout, AV_SAMPLE_FMT_S16, p.out_sample_rate,
                              &p.ctx->ch_layout, p.ctx->sample_fmt, p.ctx->sample_rate, 0, nullptr);
    if (ret < 0 || !p.swr) {
        log::error("failed to allocate resampler for AAC");
        return nullptr;
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, p.out_channels);
    ret = swr_alloc_set_opts2(&p.swr, &out_layout, AV_SAMPLE_FMT_S16, p.out_sample_rate,
                              &p.ctx->ch_layout, p.ctx->sample_fmt, p.ctx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0) {
        log::error("failed to configure resampler for AAC");
        return nullptr;
    }

    ret = swr_init(p.swr);
    if (ret < 0) {
        log::error("failed to init resampler for AAC: {}", ret);
        return nullptr;
    }

    p.packet = av_packet_alloc();
    p.frame = av_frame_alloc();
    if (!p.packet || !p.frame) {
        log::error("failed to allocate packet/frame for AAC");
        return nullptr;
    }

    log::info("AAC decoder created: {}Hz, {} channels", sample_rate, channels);
    return decoder;
}

std::unique_ptr<audio_decoder> audio_decoder::create_aac_lc(int sample_rate, int channels) {
    auto decoder = std::unique_ptr<audio_decoder>(new audio_decoder());
    decoder->impl_ = std::make_unique<impl>();
    auto& p = *decoder->impl_;

    p.codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    if (!p.codec) {
        log::error("AAC-LC decoder not found");
        return nullptr;
    }

    p.ctx = avcodec_alloc_context3(p.codec);
    if (!p.ctx) {
        log::error("failed to allocate AAC-LC codec context");
        return nullptr;
    }

    p.ctx->sample_rate = sample_rate;
    av_channel_layout_default(&p.ctx->ch_layout, channels);

    int ret = avcodec_open2(p.ctx, p.codec, nullptr);
    if (ret < 0) {
        log::error("failed to open AAC-LC decoder: {}", ret);
        return nullptr;
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, p.out_channels);
    ret = swr_alloc_set_opts2(&p.swr, &out_layout, AV_SAMPLE_FMT_S16, p.out_sample_rate,
                              &p.ctx->ch_layout, p.ctx->sample_fmt, p.ctx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || !p.swr) {
        log::error("failed to allocate resampler for AAC-LC");
        return nullptr;
    }

    ret = swr_init(p.swr);
    if (ret < 0) {
        log::error("failed to init resampler for AAC-LC: {}", ret);
        return nullptr;
    }

    p.packet = av_packet_alloc();
    p.frame = av_frame_alloc();
    if (!p.packet || !p.frame) {
        log::error("failed to allocate packet/frame for AAC-LC");
        return nullptr;
    }

    log::info("AAC-LC decoder created: {}Hz, {} channels", sample_rate, channels);
    return decoder;
}

std::unique_ptr<audio_decoder> audio_decoder::create_alac(int sample_rate, int channels,
                                                          std::span<const std::byte> magic_cookie) {
    auto decoder = std::unique_ptr<audio_decoder>(new audio_decoder());
    decoder->impl_ = std::make_unique<impl>();
    auto& p = *decoder->impl_;

    p.codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
    if (!p.codec) {
        log::error("ALAC decoder not found");
        return nullptr;
    }

    p.ctx = avcodec_alloc_context3(p.codec);
    if (!p.ctx) {
        log::error("failed to allocate ALAC codec context");
        return nullptr;
    }

    p.ctx->sample_rate = sample_rate;
    av_channel_layout_default(&p.ctx->ch_layout, channels);

    if (!magic_cookie.empty()) {
        p.ctx->extradata =
            static_cast<uint8_t*>(av_mallocz(magic_cookie.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!p.ctx->extradata) {
            log::error("failed to allocate ALAC extradata");
            return nullptr;
        }
        std::memcpy(p.ctx->extradata, magic_cookie.data(), magic_cookie.size());
        p.ctx->extradata_size = static_cast<int>(magic_cookie.size());
    }

    int ret = avcodec_open2(p.ctx, p.codec, nullptr);
    if (ret < 0) {
        log::error("failed to open ALAC decoder: {}", ret);
        return nullptr;
    }

    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, p.out_channels);
    ret = swr_alloc_set_opts2(&p.swr, &out_layout, AV_SAMPLE_FMT_S16, p.out_sample_rate,
                              &p.ctx->ch_layout, p.ctx->sample_fmt, p.ctx->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&out_layout);
    if (ret < 0 || !p.swr) {
        log::error("failed to allocate resampler for ALAC");
        return nullptr;
    }

    ret = swr_init(p.swr);
    if (ret < 0) {
        log::error("failed to init resampler for ALAC: {}", ret);
        return nullptr;
    }

    p.packet = av_packet_alloc();
    p.frame = av_frame_alloc();
    if (!p.packet || !p.frame) {
        log::error("failed to allocate packet/frame for ALAC");
        return nullptr;
    }

    log::info("ALAC decoder created: {}Hz, {} channels", sample_rate, channels);
    return decoder;
}

std::vector<std::byte> audio_decoder::decode(std::span<const std::byte> packet_data) {
    std::vector<std::byte> output;
    if (!impl_ || !impl_->ctx || packet_data.empty())
        return output;
    auto& p = *impl_;

    p.packet->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(packet_data.data()));
    p.packet->size = static_cast<int>(packet_data.size());

    int ret = avcodec_send_packet(p.ctx, p.packet);
    if (ret < 0) {
        log::warn("avcodec_send_packet failed: {}", ret);
        return output;
    }

    while (true) {
        ret = avcodec_receive_frame(p.ctx, p.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            log::warn("avcodec_receive_frame failed: {}", ret);
            break;
        }

        int max_out_samples = swr_get_out_samples(p.swr, p.frame->nb_samples);
        if (max_out_samples < 0)
            max_out_samples = p.frame->nb_samples;

        size_t bytes_per_sample = static_cast<size_t>(p.out_channels) * sizeof(int16_t);
        auto buf = std::vector<uint8_t>(static_cast<size_t>(max_out_samples) * bytes_per_sample);
        uint8_t* out_buf = buf.data();

        int converted =
            swr_convert(p.swr, &out_buf, max_out_samples,
                        const_cast<const uint8_t**>(p.frame->extended_data), p.frame->nb_samples);
        if (converted < 0) {
            log::warn("swr_convert failed: {}", converted);
            continue;
        }

        size_t out_bytes = static_cast<size_t>(converted) * bytes_per_sample;
        size_t prev = output.size();
        output.resize(prev + out_bytes);
        std::memcpy(output.data() + prev, buf.data(), out_bytes);

        av_frame_unref(p.frame);
    }

    p.packet->data = nullptr;
    p.packet->size = 0;

    return output;
}

}  // namespace mirage::audio
