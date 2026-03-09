#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/core.hpp"

namespace mirage::audio {

class ring_buffer {
public:
    explicit ring_buffer(size_t capacity);
    size_t write(std::span<const std::byte> data);
    size_t read(std::span<std::byte> out);
    [[nodiscard]] size_t available() const;

private:
    std::vector<std::byte> buf_;
    size_t capacity_;
    std::atomic<size_t> read_pos_{0};
    std::atomic<size_t> write_pos_{0};
};

class audio_player {
public:
    static std::unique_ptr<audio_player> create(int sample_rate, int channels);
    void push_pcm(std::span<const std::byte> pcm);
    void set_volume(float linear_volume);
    void stop();
    ~audio_player();
    audio_player(const audio_player&) = delete;
    audio_player& operator=(const audio_player&) = delete;

private:
    audio_player() = default;
    struct impl;
    std::unique_ptr<impl> impl_;
};

class audio_decoder {
public:
    static std::unique_ptr<audio_decoder> create_aac(int sample_rate, int channels);
    static std::unique_ptr<audio_decoder> create_aac_lc(int sample_rate, int channels);
    static std::unique_ptr<audio_decoder> create_alac(int sample_rate, int channels,
                                                      std::span<const std::byte> magic_cookie);
    std::vector<std::byte> decode(std::span<const std::byte> packet);
    ~audio_decoder();
    audio_decoder(const audio_decoder&) = delete;
    audio_decoder& operator=(const audio_decoder&) = delete;

private:
    audio_decoder() = default;
    struct impl;
    std::unique_ptr<impl> impl_;
};

}  // namespace mirage::audio
