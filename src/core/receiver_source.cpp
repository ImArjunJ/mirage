#include "core/receiver_source.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <utility>

#include "core/log.hpp"
#include "core/receiver_session.hpp"

namespace mirage {

result<void> receiver_source_descriptor::validate(const receiver_source_runtime& runtime) const {
    if (!enabled) {
        return {};
    }
    if (validate_source == nullptr) {
        return {};
    }
    return validate_source(*this, runtime);
}

result<std::unique_ptr<receiver_session>> receiver_source_descriptor::create_session(
    const receiver_source_runtime& runtime) const {
    if (!enabled) {
        return std::unexpected(
            mirage_error::session(std::format("{} receiver source is disabled", protocol_id(id))));
    }

    if (auto valid = validate(runtime); !valid) {
        return std::unexpected(valid.error());
    }

    if (session_factory == nullptr) {
        return std::unexpected(mirage_error::internal(
            std::format("{} receiver source has no session factory", protocol_id(id))));
    }

    return session_factory(*this, runtime);
}

receiver_stream_health classify_audio_stream(const receiver_audio_stream_summary& summary) {
    if (summary.gaps == 0 && summary.resend_requests == 0 && summary.invalid == 0 &&
        summary.pending == 0) {
        return receiver_stream_health::clean;
    }
    return receiver_stream_health::attention;
}

receiver_stream_health classify_video_stream(const receiver_video_stream_summary& summary) {
    if (summary.frames > 0 && summary.keyframes > 0) {
        return receiver_stream_health::clean;
    }
    return receiver_stream_health::attention;
}

void log_receiver_audio_setup(const receiver_source_descriptor& source,
                              const receiver_audio_stream_setup& setup, std::string_view label) {
    if (setup.timing_port) {
        log::diagnostic(
            "{}: codec={}, sample_rate={}, channels={}, spf={}, data_port={}, "
            "control_port={}, timing_port={}, source={}, transport={}",
            label, setup.codec, setup.sample_rate, setup.channels, setup.frames_per_packet,
            setup.data_port, setup.control_port, *setup.timing_port, protocol_id(source.id),
            source.capabilities.transport);
        return;
    }

    log::diagnostic(
        "{}: codec={}, sample_rate={}, channels={}, spf={}, data_port={}, control_port={}, "
        "source={}, transport={}",
        label, setup.codec, setup.sample_rate, setup.channels, setup.frames_per_packet,
        setup.data_port, setup.control_port, protocol_id(source.id), source.capabilities.transport);
}

void log_receiver_audio_summary(const receiver_source_descriptor& source,
                                const receiver_audio_stream_summary& summary) {
    log::diagnostic(
        "Audio stream summary: health={}, decoded_packets={}, silent_or_marker={}, gaps={}, "
        "resend_requests={}, stale_or_redundant={}, duplicates={}, invalid={}, pending={}, "
        "source={}, transport={}",
        to_string(classify_audio_stream(summary)), summary.decoded_packets,
        summary.silent_or_marker, summary.gaps, summary.resend_requests, summary.stale_or_redundant,
        summary.duplicates, summary.invalid, summary.pending, protocol_id(source.id),
        source.capabilities.transport);
}

void log_receiver_video_summary(const receiver_source_descriptor& source,
                                const receiver_video_stream_summary& summary) {
    log::diagnostic(
        "Video stream summary: health={}, frames={}, keyframes={}, source={}, transport={}",
        to_string(classify_video_stream(summary)), summary.frames, summary.keyframes,
        protocol_id(source.id), source.capabilities.transport);
}

receiver_source_registry::receiver_source_registry(std::vector<receiver_source_descriptor> sources)
    : sources_(std::move(sources)) {}

std::span<const receiver_source_descriptor> receiver_source_registry::all() const {
    return {sources_.data(), sources_.size()};
}

const receiver_source_descriptor* receiver_source_registry::find(protocol id) const {
    auto it = std::ranges::find(sources_, id, &receiver_source_descriptor::id);
    if (it == sources_.end()) {
        return nullptr;
    }
    return &*it;
}

std::vector<receiver_source_descriptor> receiver_source_registry::enabled() const {
    std::vector<receiver_source_descriptor> result;
    for (const auto& source : sources_) {
        if (source.enabled) {
            result.push_back(source);
        }
    }
    return result;
}

}  // namespace mirage
