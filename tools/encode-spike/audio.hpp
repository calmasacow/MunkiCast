// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::spike {

struct AudioConfig {
    std::uint32_t rate = 48000;
    std::uint32_t channels = 2;
    /// Capture a sink's monitor (desktop audio) rather than a source (microphone).
    bool capture_sink = true;
    /// Node name or serial to connect to; empty = whatever PipeWire considers default.
    std::string target;
};

struct AudioCapture;
void audio_close(AudioCapture*);
struct AudioDeleter {
    void operator()(AudioCapture* p) const { audio_close(p); }
};
using AudioPtr = std::unique_ptr<AudioCapture, AudioDeleter>;

/// Starts capture on its own PipeWire thread loop. Audio must never be gated on the video path -
/// it is the master clock, and a stalled compositor must not be able to starve it.
AudioPtr audio_open(const AudioConfig& cfg, std::string& error);

/// Moves everything captured so far into `out` as interleaved float32, and reports how many
/// sample-frames that is. Returns false only on a hard error.
bool audio_drain(AudioCapture& cap, std::vector<float>& out, std::uint32_t& frames);

/// Sample-frames the PipeWire side has produced since start, including any not yet drained.
/// This is the number that reveals an underrun: it should advance at exactly the sample rate.
std::uint64_t audio_total_frames(AudioCapture& cap);

/// True once the stream has reached PW_STREAM_STATE_STREAMING at least once.
bool audio_running(AudioCapture& cap);

std::string audio_format_str(AudioCapture& cap);

} // namespace munkicast::spike
