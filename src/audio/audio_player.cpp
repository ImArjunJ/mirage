#include <algorithm>
#include <bit>
#include <cstring>

#include "audio/audio.hpp"
#include "core/log.hpp"
#include "platform/audio_out.hpp"

namespace mirage::audio {

ring_buffer::ring_buffer(size_t capacity) : capacity_(std::bit_ceil(capacity)) {
    buf_.resize(capacity_);
}

size_t ring_buffer::write(std::span<const std::byte> data) {
    auto rp = read_pos_.load(std::memory_order_acquire);
    auto wp = write_pos_.load(std::memory_order_relaxed);
    size_t free = capacity_ - (wp - rp);
    size_t to_write = std::min(data.size(), free);
    if (to_write == 0)
        return 0;

    size_t pos = wp & (capacity_ - 1);
    size_t first = std::min(to_write, capacity_ - pos);
    std::memcpy(buf_.data() + pos, data.data(), first);
    if (first < to_write) {
        std::memcpy(buf_.data(), data.data() + first, to_write - first);
    }
    write_pos_.store(wp + to_write, std::memory_order_release);
    return to_write;
}

size_t ring_buffer::read(std::span<std::byte> out) {
    auto wp = write_pos_.load(std::memory_order_acquire);
    auto rp = read_pos_.load(std::memory_order_relaxed);
    size_t avail = wp - rp;
    size_t to_read = std::min(out.size(), avail);
    if (to_read == 0)
        return 0;

    size_t pos = rp & (capacity_ - 1);
    size_t first = std::min(to_read, capacity_ - pos);
    std::memcpy(out.data(), buf_.data() + pos, first);
    if (first < to_read) {
        std::memcpy(out.data() + first, buf_.data(), to_read - first);
    }
    read_pos_.store(rp + to_read, std::memory_order_release);
    return to_read;
}

size_t ring_buffer::available() const {
    return write_pos_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire);
}

struct audio_player::impl {
    std::unique_ptr<platform::audio_output> output;
    ring_buffer buffer{65536};
};

std::unique_ptr<audio_player> audio_player::create(int sample_rate, int channels) {
    auto player = std::unique_ptr<audio_player>(new audio_player());
    player->impl_ = std::make_unique<impl>();

    player->impl_->output = platform::audio_output::create(
        sample_rate, channels, [&buf = player->impl_->buffer](std::span<std::byte> out) -> size_t {
            return buf.read(out);
        });

    if (!player->impl_->output) {
        log::error("failed to create audio output");
        return nullptr;
    }

    log::info("audio player created: {}Hz, {} channels", sample_rate, channels);
    return player;
}

void audio_player::push_pcm(std::span<const std::byte> pcm) {
    if (impl_ && impl_->output) {
        impl_->buffer.write(pcm);
    }
}

void audio_player::set_volume(float linear_volume) {
    if (impl_ && impl_->output) {
        impl_->output->set_volume(linear_volume);
    }
}

void audio_player::stop() {
    if (impl_ && impl_->output) {
        impl_->output->stop();
        impl_->output.reset();
        log::info("audio player stopped");
    }
}

audio_player::~audio_player() {
    stop();
}

}  // namespace mirage::audio
