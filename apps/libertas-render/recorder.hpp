// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio_sources.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <lcomp/composite.hpp>
#include <lcomp/preview.hpp>
#include <lcore/log.hpp>
#include <ldoc/document.hpp>
#include <lencode/encoder.hpp>
#include <string>

/// One shared encode feeding independently controlled recording and RTMP destinations.
///
/// Video and audio are stamped off one clock -- the wall clock at the first frame -- because
/// ADR-0017 does not care which clock the muxer is given, only that there is exactly one.
class Recorder {
  public:
    explicit Recorder(AudioSources& audio) : audio_(audio) {}

    ~Recorder() {
        const auto error = stop();
        if (!error.empty())
            lc::error("Recording did not close cleanly: {}", error);
    }

    [[nodiscard]] bool running() const { return encoder_ != nullptr; }

    /// The compositor's Vulkan device UUID, so NVENC opens on the same GPU and reads its frames
    /// in place. Zero (or unset) keeps system-memory frames.
    std::function<std::array<std::uint8_t, 16>()> gpu_uuid;

    /// Whether the running encoder takes GPU-resident NV12 frames instead of RGBA pixels.
    [[nodiscard]] bool takes_device_frames() const {
        return encoder_ && libertas::encode::encoder_takes_device_frames(*encoder_);
    }

    /// Which canvas is being recorded, or nil. The preview loop asks this to decide whether a
    /// canvas has to be composited at full size as well as preview size.
    [[nodiscard]] const ldoc::Id& canvas() const { return canvas_; }

    lcomp::RecordState control(const lcomp::RecordRequest& request, const ldoc::Project& project) {
        using lcomp::RecordOperation;
        if (request.operation == RecordOperation::ReplaySave) {
            std::string error;
            if (!encoder_ || !replay_output_)
                error = "Start the replay buffer first.";
            else
                libertas::encode::encoder_save_replay(*encoder_, request.path, error);
            auto state = status(running(), error);
            if (!error.empty())
                state.clip_path = request.path;
            return state;
        }
        if (request.operation == RecordOperation::ReplayStop) {
            if (encoder_ && libertas::encode::encoder_replay_stats(*encoder_).saving)
                return status(running(), "Wait for the replay clip to finish saving.");
            if (replay_output_) {
                if (!recording_output_ && !streaming_output_)
                    return status(false, stop());
                std::string error;
                libertas::encode::encoder_set_replay(*encoder_, 0, 0, error);
                replay_output_.reset();
            }
            return status(running(), {});
        }
        if (!request.start) {
            std::string error;
            auto& target = request.output.is_nil() ? recording_output_ : streaming_output_;
            if ((request.encode.is_nil() && request.output.is_nil()) ||
                (target && !replay_output_ &&
                 !(request.output.is_nil() ? streaming_output_ : recording_output_))) {
                error = stop();
            } else if (target && encoder_) {
                libertas::encode::encoder_remove_output(*encoder_, *target, error);
                target.reset();
            }
            return status(running(), error);
        }

        const ldoc::Encode* encode = project.encode(request.encode);
        if (!encode)
            return status(running(), "that encode is not in this project");
        const ldoc::Canvas* canvas = project.canvas(encode->canvas);
        if (!canvas)
            return status(running(),
                          std::format("encode '{}' has no canvas to record", encode->name));

        const ldoc::Output* destination = nullptr;
        for (const auto& o : project.outputs) {
            if ((!request.output.is_nil() && o.id == request.output) ||
                (request.output.is_nil() && o.encode == encode->id &&
                 o.destination.kind == ldoc::DestinationKind::File)) {
                destination = &o;
                break;
            }
        }
        const bool replay = request.operation == RecordOperation::ReplayStart;
        if (replay &&
            (!destination || destination->destination.kind != ldoc::DestinationKind::Replay ||
             !destination->enabled || destination->encode != encode->id))
            return status(running(), "Enable a connected Replay destination first.");
        const bool streaming = !replay && !request.output.is_nil();
        if (streaming &&
            (!destination || !destination->enabled || destination->encode != encode->id ||
             (destination->destination.kind != ldoc::DestinationKind::Rtmp &&
              destination->destination.kind != ldoc::DestinationKind::Rtmps)))
            return status(running(), "Choose an armed RTMP or RTMPS destination.");
        const bool network =
            request.path.starts_with("rtmp://") || request.path.starts_with("rtmps://");
        if (network != streaming)
            return status(running(), "The output address does not match its destination.");
        const ldoc::AudioEncode* audio = destination && destination->audio_encode
                                             ? project.audio_encode(*destination->audio_encode)
                                             : nullptr;
        // Legacy recording callers without a file destination use the first audio route.
        if (!destination && !streaming)
            for (const auto& o : project.outputs)
                if (o.encode == encode->id && o.audio_encode) {
                    audio = project.audio_encode(*o.audio_encode);
                    break;
                }
        if (streaming && (encode->encoder.codec != "h264" || !audio || audio->codec != "aac" ||
                          !project.bus(audio->bus)))
            return status(running(),
                          "Basic RTMP needs H.264 video and a connected AAC audio encoder.");
        ldoc::AudioEncode silent;
        silent.bitrate_kbps = 0;

        auto cfg = libertas::encode::runtime_config(*encode, audio ? *audio : silent, *canvas,
                                                    request.path);
        if (gpu_uuid)
            cfg.gpu_uuid = gpu_uuid();
        if (!audio) {
            cfg.audio_channels = 0; // no audio stream at all, rather than a silent one
            cfg.audio_bitrate = 0;
        }

        auto& target = replay ? replay_output_ : streaming ? streaming_output_ : recording_output_;
        if (target)
            return status(running(), "That destination is already running.");
        const auto audio_id = audio ? audio->id : ldoc::Id::nil();
        if (encoder_) {
            if (encode->id != encode_ || audio_id != audio_encode_)
                return status(true,
                              "Concurrent destinations must share their video and audio encodes.");
            std::string error;
            if (replay) {
                if (libertas::encode::encoder_set_replay(
                        *encoder_, destination->destination.replay_seconds,
                        static_cast<std::uint64_t>(destination->destination.replay_memory_mb) *
                            1048576,
                        error))
                    target = 0;
            } else
                target = libertas::encode::encoder_add_output(*encoder_, request.path, error);
            if (target && streaming)
                streaming_id_ = request.output;
            return status(true, error);
        }
        std::string error;
        encoder_ = libertas::encode::encoder_open(cfg, error);
        if (!encoder_) {
            canvas_ = ldoc::Id::nil();
            audio_.tap(ldoc::Id::nil());
            return {false, error.empty() ? std::string{"the encoder would not open"} : error};
        }
        if (replay &&
            !libertas::encode::encoder_set_replay(
                *encoder_, destination->destination.replay_seconds,
                static_cast<std::uint64_t>(destination->destination.replay_memory_mb) * 1048576,
                error)) {
            encoder_.reset();
            return status(false, error);
        }
        target = 0;
        encode_ = encode->id;
        audio_encode_ = audio_id;
        if (streaming)
            streaming_id_ = request.output;
        canvas_ = canvas->id;
        width_ = cfg.source_width;
        height_ = cfg.source_height;
        frame_rate_ = static_cast<double>(cfg.fps_num) / cfg.fps_den;
        video_origin_ = -1;
        last_video_slot_ = -1;
        frames_ = missed_ = encode_lag_ = encode_blocked_slots_ = 0;
        started_ = std::chrono::steady_clock::now();
        audio_.tap(audio ? audio->bus : ldoc::Id::nil());
        lc::info("{} {}", streaming ? "streaming" : "recording", encode->name);
        return status(true, {});
    }

    /// One composited frame; report a failed recording back over the preview channel.
    std::optional<lcomp::RecordState> frame(const ldoc::Id& canvas, const lcomp::Picture& picture,
                                            std::chrono::steady_clock::time_point frame_time) {
        if (!encoder_ || canvas != canvas_)
            return std::nullopt;
        std::string error;
        if (picture.width != static_cast<std::int32_t>(width_) ||
            picture.height != static_cast<std::int32_t>(height_)) {
            error = "Output stopped because the canvas size changed.";
        } else if (const auto* pixels = picture.nv12 ? nullptr : picture.read_from();
                   pixels || (picture.nv12 && takes_device_frames())) {
            const auto pts =
                std::chrono::duration_cast<std::chrono::nanoseconds>(frame_time - started_).count();
            // Match the encoder time base; early ticks must not submit duplicate timestamps.
            if (video_origin_ < 0)
                video_origin_ = pts;
            const auto slot =
                std::llround(static_cast<double>(pts - video_origin_) * frame_rate_ / 1e9);
            if (slot > last_video_slot_) {
                const auto gap = static_cast<std::uint64_t>(slot - last_video_slot_ - 1);
                missed_ += gap;
                encode_lag_ += std::min(gap, encode_blocked_slots_);
                const auto encode_started = std::chrono::steady_clock::now();
                const auto& gpu = picture.nv12;
                if (gpu ? libertas::encode::encoder_video_device(*encoder_, gpu->fd, gpu->size,
                                                                 gpu->id, gpu->pitch, gpu, pts,
                                                                 error)
                        : libertas::encode::encoder_video(*encoder_, pixels, picture.stride, pts,
                                                          error)) {
                    last_video_slot_ = slot;
                    ++frames_;
                }
                if (error.empty())
                    pump_audio(pts, error);
                const auto finished = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started_)
                                          .count();
                const auto end_slot =
                    std::llround(static_cast<double>(finished - video_origin_) * frame_rate_ / 1e9);
                const auto encode_started_ns =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(encode_started - started_)
                        .count();
                if (std::getenv("LIBERTAS_BENCHMARK") && error.empty()) {
                    const auto& stats = libertas::encode::encoder_stats(*encoder_);
                    if (!stats.convert_ms.empty() && !stats.video_encode_ms.empty())
                        std::fprintf(stderr,
                                     "OUTPUT_ENCODE sample_ns=%lld convert_ms=%.3f codec_ms=%.3f "
                                     "other_ms=%.3f\n",
                                     static_cast<long long>(
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             frame_time.time_since_epoch())
                                             .count()),
                                     stats.convert_ms.back(), stats.video_encode_ms.back(),
                                     static_cast<double>(finished - encode_started_ns) / 1e6 -
                                         stats.convert_ms.back() - stats.video_encode_ms.back());
                }
                const auto encode_slot = std::llround(
                    static_cast<double>(encode_started_ns - video_origin_) * frame_rate_ / 1e9);
                encode_blocked_slots_ = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, end_slot - encode_slot - 1));
            }
        } else {
            error = "Output stopped because the rendered frame is unavailable.";
        }
        if (!error.empty()) {
            const auto finish_error = stop();
            if (!finish_error.empty())
                error += " " + finish_error;
            auto state = status(false, error);
            state.control_reply = false;
            return state;
        }
        for (auto* output : {&recording_output_, &streaming_output_}) {
            if (*output && !libertas::encode::encoder_output_active(*encoder_, **output)) {
                const auto failure = libertas::encode::encoder_output_error(*encoder_, **output);
                if (!error.empty())
                    error += " ";
                error += failure;
                output->reset();
            }
        }
        if (const auto saved = libertas::encode::encoder_poll_replay(*encoder_)) {
            auto state = status(true, saved->error);
            state.clip_path = saved->path;
            state.control_reply = false;
            return state;
        }
        if (error.empty() || recording_output_ || streaming_output_ || replay_output_) {
            auto state = status(true, error);
            state.control_reply = false;
            return state;
        }
        const auto finish_error = stop();
        if (!finish_error.empty())
            error += " " + finish_error;
        lc::error("{}", error);
        return status(false, error);
    }

    /// Flush even after failure, then release the file and audio route.
    std::string stop() {
        if (!encoder_)
            return {};
        std::string error;
        pump_audio(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - started_)
                       .count(),
                   error);
        std::string finish_error;
        if (!libertas::encode::encoder_finish(*encoder_, finish_error)) {
            if (!error.empty())
                error += " ";
            error += finish_error;
        }
        encoder_.reset();
        recording_output_.reset();
        streaming_output_.reset();
        replay_output_.reset();
        canvas_ = ldoc::Id::nil();
        audio_.tap(ldoc::Id::nil());
        return error;
    }

  private:
    lcomp::RecordState status(bool running, const std::string& error) const {
        lcomp::RecordState state{running,
                                 error,
                                 frames_,
                                 missed_,
                                 encode_lag_,
                                 recording_output_.has_value(),
                                 streaming_output_.has_value()};
        if (encoder_) {
            const auto replay = libertas::encode::encoder_replay_stats(*encoder_);
            state.replay = replay.active;
            state.replay_saving = replay.saving;
            state.replay_bytes = replay.bytes;
            state.replay_duration_ms = replay.duration_ms;
        }
        return state;
    }

    void pump_audio(std::int64_t pts_ns, std::string& error) {
        auto samples = audio_.drain_tap();
        if (samples.empty())
            return;
        const auto frames = static_cast<std::uint32_t>(samples.size() / 2);
        libertas::encode::encoder_audio(*encoder_, samples.data(), frames, pts_ns, error);
    }

    AudioSources& audio_;
    libertas::encode::EncoderPtr encoder_;
    std::optional<std::size_t> recording_output_, streaming_output_, replay_output_;
    ldoc::Id encode_ = ldoc::Id::nil(), audio_encode_ = ldoc::Id::nil();
    ldoc::Id streaming_id_ = ldoc::Id::nil();
    ldoc::Id canvas_ = ldoc::Id::nil();
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::chrono::steady_clock::time_point started_{};
    std::uint64_t frames_ = 0, missed_ = 0, encode_lag_ = 0, encode_blocked_slots_ = 0;
    double frame_rate_ = 0;
    std::int64_t video_origin_ = -1;
    std::int64_t last_video_slot_ = -1;
};
