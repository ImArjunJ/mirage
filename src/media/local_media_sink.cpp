#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "audio/audio.hpp"
#include "core/log.hpp"
#include "media/media.hpp"
#include "media/pipeline.hpp"
#include "render/render.hpp"

namespace mirage::media {
namespace {

class local_media_sink final : public media_sink {
public:
    result<void> on_audio_setup(const audio_stream_config& config) override {
        audio_config_ = config;
        audio_decoder_.reset();
        if (audio_player_) {
            audio_player_->stop();
        }
        audio_player_.reset();

        if (config.codec == audio_codec::alac) {
            audio_decoder_ = audio::audio_decoder::create_alac(config.sample_rate, config.channels,
                                                               config.codec_config);
        } else {
            audio_decoder_ = audio::audio_decoder::create_aac(config.sample_rate, config.channels);
        }
        if (!audio_decoder_) {
            return std::unexpected(mirage_error::decode("failed to create audio decoder"));
        }

        audio_player_ = audio::audio_player::create(config.sample_rate, config.channels);
        if (!audio_player_) {
            return std::unexpected(mirage_error::decode("failed to create audio player"));
        }
        audio_player_->set_volume(audio_linear_volume_);
        log::info("Audio decoder and player initialized");
        return {};
    }

    result<void> on_audio_packet(const media_packet& packet) override {
        if (!audio_decoder_) {
            return std::unexpected(mirage_error::decode("audio stream is not configured"));
        }
        auto pcm = audio_decoder_->decode(packet.payload);
        if (!pcm.empty() && audio_player_) {
            audio_player_->push_pcm(pcm);
        }
        return {};
    }

    void on_audio_flush() override {}

    void on_audio_pause() override {
        if (audio_player_) {
            audio_player_->stop();
        }
    }

    void on_audio_teardown() override {
        if (audio_player_) {
            audio_player_->stop();
        }
        audio_player_.reset();
        audio_decoder_.reset();
    }

    void on_audio_volume(float db, float linear) override {
        audio_volume_db_ = db;
        audio_linear_volume_ = linear;
        if (audio_player_) {
            audio_player_->set_volume(linear);
        }
    }

    result<void> on_video_setup(const video_stream_config& config) override {
        video_config_ = config;
        video_decoder_.reset();
        video_renderer_.reset();

        auto decoder = unified_decoder::create(config.codec, config.prefer_hardware);
        if (decoder) {
            video_decoder_ = std::move(*decoder);
            log::info("Video decoder initialized: {}", video_decoder_->backend_name());
        } else {
            log::warn("Failed to create video decoder: {}", decoder.error().message);
        }

        auto title = config.title.empty() ? std::string("Mirage") : config.title;
        video_renderer_ = render::render_window::create(title, config.width, config.height);
        return {};
    }

    result<void> on_video_packet(const media_packet& packet) override {
        if (!video_decoder_ || !video_renderer_ || !video_renderer_->is_open()) {
            return {};
        }
        auto decoded = video_decoder_->decode(packet.payload);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        if (decoded->has_value()) {
            video_renderer_->submit_frame(std::move(decoded->value()));
        }
        return {};
    }

    [[nodiscard]] bool video_open() const override {
        return !video_renderer_ || video_renderer_->is_open();
    }

    void on_video_teardown() override {
        video_renderer_.reset();
        video_decoder_.reset();
    }

private:
    audio_stream_config audio_config_;
    video_stream_config video_config_;
    std::unique_ptr<audio::audio_player> audio_player_;
    std::unique_ptr<audio::audio_decoder> audio_decoder_;
    std::optional<unified_decoder> video_decoder_;
    std::unique_ptr<render::render_window> video_renderer_;
    float audio_volume_db_ = 0.0F;
    float audio_linear_volume_ = 1.0F;
};

}  // namespace

std::unique_ptr<media_sink> make_local_media_sink() {
    return std::make_unique<local_media_sink>();
}

}  // namespace mirage::media
