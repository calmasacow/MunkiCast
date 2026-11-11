// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PipeWire audio capture for the Phase-0 gate.
//
// Runs on its own thread loop deliberately. Audio is the master clock, so it must keep advancing
// while the video path is blocked on a compositor that has not damaged the screen - and under
// ADR-0013 that is the normal case, not an error.

#include "audio.hpp"

#include <lcore/log.hpp>

#include <cstring>
#include <format>
#include <mutex>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

namespace munkicast::spike {

struct AudioCapture {
    AudioConfig cfg;
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};

    std::mutex mu;
    std::vector<float> pending; ///< interleaved
    std::uint64_t total_frames = 0;
    bool running = false;
    std::uint32_t got_rate = 0;
    std::uint32_t got_channels = 0;
};

namespace {

void on_state(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* err) {
    auto* cap = static_cast<AudioCapture*>(data);
    if (state == PW_STREAM_STATE_STREAMING) {
        std::lock_guard<std::mutex> lk(cap->mu);
        cap->running = true;
    } else if (state == PW_STREAM_STATE_ERROR) {
        lc::error("audio stream error: {}", err != nullptr ? err : "(none)");
    }
}

void on_param_changed(void* data, std::uint32_t id, const spa_pod* param) {
    auto* cap = static_cast<AudioCapture*>(data);
    if (param == nullptr || id != SPA_PARAM_Format)
        return;
    spa_audio_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
        return;
    if (info.media_type != SPA_MEDIA_TYPE_audio || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
        return;
    std::lock_guard<std::mutex> lk(cap->mu);
    cap->got_rate = info.info.raw.rate;
    cap->got_channels = info.info.raw.channels;
    lc::info("audio negotiated: {} Hz, {} ch", cap->got_rate, cap->got_channels);
}

void on_process(void* data) {
    auto* cap = static_cast<AudioCapture*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(cap->stream);
    if (b == nullptr)
        return;
    spa_buffer* buf = b->buffer;
    const auto* samples = static_cast<const float*>(buf->datas[0].data);
    if (samples != nullptr && buf->datas[0].chunk != nullptr) {
        const std::uint32_t stride = sizeof(float) * cap->cfg.channels;
        const std::uint32_t n = buf->datas[0].chunk->size / stride;
        if (n > 0) {
            std::lock_guard<std::mutex> lk(cap->mu);
            cap->pending.insert(cap->pending.end(), samples,
                                samples + static_cast<std::size_t>(n) * cap->cfg.channels);
            cap->total_frames += n;
        }
    }
    pw_stream_queue_buffer(cap->stream, b);
}

/// Built by assignment rather than with designated initializers.
///
/// PipeWire's own idiom is a designated initializer, but `pw_stream_events` gains callbacks
/// between releases, and every field left out is a -Wmissing-field-initializers diagnostic --
/// which is an error under the CI preset. Zero-initialising and assigning the four we implement
/// says the same thing and keeps saying it when PipeWire adds a fifth.
pw_stream_events make_stream_events() {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = on_state;
    e.param_changed = on_param_changed;
    e.process = on_process;
    return e;
}

const pw_stream_events kEvents = make_stream_events();

} // namespace

AudioPtr audio_open(const AudioConfig& cfg, std::string& error) {
    static bool inited = false;
    if (!inited) {
        pw_init(nullptr, nullptr);
        inited = true;
    }

    auto cap = AudioPtr(new AudioCapture());
    cap->cfg = cfg;

    cap->loop = pw_thread_loop_new("munkicast-audio", nullptr);
    if (cap->loop == nullptr) {
        error = "pw_thread_loop_new failed";
        return nullptr;
    }

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Production", nullptr);
    if (cfg.capture_sink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    if (!cfg.target.empty())
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg.target.c_str());

    pw_thread_loop_lock(cap->loop);
    cap->stream = pw_stream_new_simple(pw_thread_loop_get_loop(cap->loop), "munkicast-capture",
                                       props, &kEvents, cap.get());
    if (cap->stream == nullptr) {
        pw_thread_loop_unlock(cap->loop);
        error = "pw_stream_new_simple failed";
        return nullptr;
    }

    std::uint8_t pod_buf[1024];
    spa_pod_builder bld = SPA_POD_BUILDER_INIT(pod_buf, sizeof pod_buf);
    spa_audio_info_raw raw{};
    raw.format = SPA_AUDIO_FORMAT_F32;
    raw.rate = cfg.rate;
    raw.channels = cfg.channels;
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&bld, SPA_PARAM_EnumFormat, &raw);

    const int r = pw_stream_connect(
        cap->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                     PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    pw_thread_loop_unlock(cap->loop);
    if (r < 0) {
        error = std::format("pw_stream_connect: {}", std::strerror(-r));
        return nullptr;
    }
    if (pw_thread_loop_start(cap->loop) < 0) {
        error = "pw_thread_loop_start failed";
        return nullptr;
    }
    return cap;
}

bool audio_drain(AudioCapture& cap, std::vector<float>& out, std::uint32_t& frames) {
    std::lock_guard<std::mutex> lk(cap.mu);
    out.swap(cap.pending);
    cap.pending.clear();
    frames = cap.cfg.channels > 0
                 ? static_cast<std::uint32_t>(out.size() / cap.cfg.channels)
                 : 0;
    return true;
}

std::uint64_t audio_total_frames(AudioCapture& cap) {
    std::lock_guard<std::mutex> lk(cap.mu);
    return cap.total_frames;
}

bool audio_running(AudioCapture& cap) {
    std::lock_guard<std::mutex> lk(cap.mu);
    return cap.running;
}

std::string audio_format_str(AudioCapture& cap) {
    std::lock_guard<std::mutex> lk(cap.mu);
    return std::format("{} Hz / {} ch", cap.got_rate, cap.got_channels);
}

void audio_close(AudioCapture* cap) {
    if (cap == nullptr)
        return;
    if (cap->loop != nullptr)
        pw_thread_loop_stop(cap->loop);
    if (cap->stream != nullptr)
        pw_stream_destroy(cap->stream);
    if (cap->loop != nullptr)
        pw_thread_loop_destroy(cap->loop);
    delete cap;
}

} // namespace munkicast::spike
