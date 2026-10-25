// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio_sources.hpp"

#include <lcomp/composite.hpp>
#include <lcomp/preview.hpp>
#include <lcore/log.hpp>
#include <ldoc/document.hpp>
#include <lencode/encoder.hpp>

#include <chrono>
#include <cmath>
#include <string>

/// Writing one encode to one file.
///
/// One at a time, deliberately. The encoder ladder picks a real device (ADR-0006) and two
/// recordings would contend for it; the shape that wants several is several encodes going to
/// several destinations, which is the streaming path and is not built yet.
///
/// Video and audio are stamped off one clock -- the wall clock at the first frame -- because
/// ADR-0017 does not care which clock the muxer is given, only that there is exactly one.
class Recorder {
  public:
    explicit Recorder(AudioSources& audio) : audio_(audio) {}

    ~Recorder() {
        const auto error = stop();
        if (!error.empty()) lc::error("Recording did not close cleanly: {}", error);
    }

    [[nodiscard]] bool running() const { return encoder_ != nullptr; }

    /// Which canvas is being recorded, or nil. The preview loop asks this to decide whether a
    /// canvas has to be composited at full size as well as preview size.
    [[nodiscard]] const ldoc::Id& canvas() const { return canvas_; }

    lcomp::RecordState control(const lcomp::RecordRequest& request,
                               const ldoc::Project& project) {
        if (!request.start) {
            return {false, stop()};
        }
        if (encoder_)
            return {true, "A recording is already running; stop it before starting another."};

        const ldoc::Encode* encode = project.encode(request.encode);
        if (!encode)
            return {false, "that encode is not in this project"};
        const ldoc::Canvas* canvas = project.canvas(encode->canvas);
        if (!canvas)
            return {false, std::format("encode '{}' has no canvas to record", encode->name)};

        // A file destination owns its audio route, including an intentionally silent route.
        // Legacy callers without a file output retain the first connected audio route.
        const ldoc::AudioEncode* audio = nullptr;
        for (const ldoc::Output& o : project.outputs) {
            if (o.encode == encode->id && o.destination.kind == ldoc::DestinationKind::File) {
                audio = o.audio_encode ? project.audio_encode(*o.audio_encode) : nullptr;
                break;
            }
            if (!audio && o.encode == encode->id && o.audio_encode) {
                audio = project.audio_encode(*o.audio_encode);
            }
        }
        ldoc::AudioEncode silent;
        silent.bitrate_kbps = 0;

        auto cfg = libertas::encode::runtime_config(*encode, audio ? *audio : silent,
                                                    *canvas, request.path);
        if (!audio) {
            cfg.audio_channels = 0; // no audio stream at all, rather than a silent one
            cfg.audio_bitrate = 0;
        }

        std::string error;
        encoder_ = libertas::encode::encoder_open(cfg, error);
        if (!encoder_) {
            canvas_ = ldoc::Id::nil();
            audio_.tap(ldoc::Id::nil());
            return {false, error.empty() ? std::string{"the encoder would not open"} : error};
        }
        canvas_ = canvas->id;
        width_ = cfg.source_width;
        height_ = cfg.source_height;
        frame_rate_ = static_cast<double>(cfg.fps_num) / cfg.fps_den;
        video_origin_ = -1;
        last_video_slot_ = -1;
        started_ = std::chrono::steady_clock::now();
        audio_.tap(audio ? audio->bus : ldoc::Id::nil());
        lc::info("recording {} to {}", encode->name, request.path);
        return {true, {}};
    }

    /// One composited frame; report a failed recording back over the preview channel.
    std::optional<lcomp::RecordState> frame(const ldoc::Id& canvas,
                                           const lcomp::Picture& picture) {
        if (!encoder_ || canvas != canvas_)
            return std::nullopt;
        std::string error;
        if (picture.width != static_cast<std::int32_t>(width_) ||
            picture.height != static_cast<std::int32_t>(height_)) {
            error = "Recording stopped because the canvas size changed.";
        } else if (const auto* pixels = picture.read_from()) {
            const auto pts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started_).count();
            // Match encoder timestamp rounding; millisecond preview ticks may arrive early.
            if (video_origin_ < 0) video_origin_ = pts;
            const auto slot = std::llround(static_cast<double>(pts - video_origin_) * frame_rate_ / 1e9);
            if (slot > last_video_slot_) {
                if (libertas::encode::encoder_video(*encoder_, pixels, picture.stride, pts, error))
                    last_video_slot_ = slot;
            }
            if (error.empty()) pump_audio(pts, error);
        } else {
            error = "Recording stopped because the rendered frame is unavailable.";
        }
        if (error.empty())
            return std::nullopt;
        const auto finish_error = stop();
        if (!finish_error.empty())
            error += " " + finish_error;
        lc::error("{}", error);
        return lcomp::RecordState{false, std::move(error)};
    }

    /// Flush even after failure, then release the file and audio route.
    std::string stop() {
        if (!encoder_)
            return {};
        std::string error;
        pump_audio(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - started_).count(), error);
        std::string finish_error;
        if (!libertas::encode::encoder_finish(*encoder_, finish_error)) {
            if (!error.empty()) error += " ";
            error += finish_error;
        }
        encoder_.reset();
        canvas_ = ldoc::Id::nil();
        audio_.tap(ldoc::Id::nil());
        return error;
    }

  private:
    void pump_audio(std::int64_t pts_ns, std::string& error) {
        auto samples = audio_.drain_tap();
        if (samples.empty())
            return;
        const auto frames = static_cast<std::uint32_t>(samples.size() / 2);
        libertas::encode::encoder_audio(*encoder_, samples.data(), frames, pts_ns, error);
    }

    AudioSources& audio_;
    libertas::encode::EncoderPtr encoder_;
    ldoc::Id canvas_ = ldoc::Id::nil();
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::chrono::steady_clock::time_point started_{};
    double frame_rate_ = 0;
    std::int64_t video_origin_ = -1;
    std::int64_t last_video_slot_ = -1;
};
