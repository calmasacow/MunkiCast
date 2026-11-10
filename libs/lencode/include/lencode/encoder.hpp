// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libertas::encode {

/// Which encoder tier was actually used. ADR-0006 specifies a ladder rather than one encoder,
/// because the top rung is not available everywhere and failing to start a show is not an option.
enum class VideoTier {
    None,
    Vulkan, ///< h264_vulkan on OUR VkDevice - no interop in the hot path
    Nvenc,  ///< h264_nvenc, software frames (costs an upload)
    X264,   ///< libx264 - the floor, always present
};

const char* tier_name(VideoTier t);

/// What the runtime needs to open an encoder. Deliberately not ldoc::EncoderConfig: that one
/// is what the *document* says, in the operator's terms, and this one is what FFmpeg needs, in
/// the muxer's. `runtime_config` below is the single place the first becomes the second.
struct RuntimeConfig {
    std::string path; ///< file (container from extension), or RTMP/RTMPS URL (FLV)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Zero uses the output size. The compositor supplies RGBA at the canvas size;
    /// the capture spike supplies BGRA at the output size.
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    bool source_rgba = false;
    std::int32_t fps_num = 60;
    std::int32_t fps_den = 1;
    std::int64_t bitrate = 12'000'000;
    std::string force_tier; ///< Encoder name from tier_name(); empty = walk the ladder
    std::int32_t keyframe_interval_ms = 2000;
    std::map<std::string, std::string> video_options;
    std::string custom_options;
    std::string rescale_filter = "bilinear";

    /// Audio. Zero channels disables the audio stream entirely, which is useful for isolating a
    /// video problem but is NOT the Phase-0 gate - the gate requires synced audio.
    std::uint32_t audio_rate = 48000;
    std::uint32_t audio_channels = 2;
    std::int64_t audio_bitrate = 160'000;
    std::string audio_codec = "aac";
};

struct EncoderStats {
    VideoTier tier = VideoTier::None;
    std::string video_encoder;
    std::string audio_encoder;
    std::uint64_t video_frames = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t video_bytes = 0;
    std::uint64_t audio_bytes = 0;
    /// Wall time spent inside avcodec_send_frame/receive_packet, which is the number that says
    /// whether this encoder can hold the show's frame budget.
    std::vector<double> video_encode_ms;
    std::vector<double> convert_ms;
    std::vector<double> copy_ms;
};

struct Encoder;
void encoder_close(Encoder*);

struct EncoderDeleter {
    void operator()(Encoder* p) const { encoder_close(p); }
};

using EncoderPtr = std::unique_ptr<Encoder, EncoderDeleter>;

/// Opens the output file and brings up both streams. Walks the encoder ladder unless pinned.
EncoderPtr encoder_open(const RuntimeConfig& cfg, std::string& error);

/// Add a muxer sharing the existing compressed streams; starts on the next keyframe.
/// Destination 0 is the original output. Removing a destination leaves other outputs running.
/// Packet writes run on bounded per-destination queues. Poll active/error for asynchronous
/// failures; removing/finishing drains queued packets and reports write/close failures.
std::optional<std::size_t> encoder_add_output(Encoder& enc, const std::string& path,
                                              std::string& error);
bool encoder_remove_output(Encoder& enc, std::size_t id, std::string& error);
bool encoder_output_active(const Encoder& enc, std::size_t id);
std::string encoder_output_error(const Encoder& enc, std::size_t id);

struct ReplayStats {
    std::uint64_t bytes = 0, duration_ms = 0;
    bool active = false, saving = false;
};

struct ReplayResult {
    std::string path, error;
};

/// Zero seconds disables buffering. Stored packets share this encode with every other output.
bool encoder_set_replay(Encoder& enc, std::uint32_t seconds, std::uint64_t bytes,
                        std::string& error);
ReplayStats encoder_replay_stats(const Encoder& enc);
bool encoder_save_replay(Encoder& enc, const std::string& path, std::string& error);
std::optional<ReplayResult> encoder_poll_replay(Encoder& enc);

/// Submits one BGRA frame, or RGBA when source_rgba is set in RuntimeConfig.
/// `pts_ns` is on the SAME clock as encoder_audio's - see ADR-0017: the
/// muxer does not care which clock, only that there is exactly one.
bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error);

/// Submits interleaved float32 audio. `frames` is sample-frames, not bytes.
bool encoder_audio(Encoder& enc, const float* interleaved, std::uint32_t frames,
                   std::int64_t pts_ns, std::string& error);

/// Flushes both encoders and writes the trailer. A file is only playable after this.
bool encoder_finish(Encoder& enc, std::string& error);

const EncoderStats& encoder_stats(Encoder& enc);

/// Submits a frame that is already in the encoder's NV12 layout, skipping colour conversion
/// entirely. This is the GPU-convert path of ADR-0019.
bool encoder_video_nv12(Encoder& enc, const std::uint8_t* y, int y_stride, const std::uint8_t* uv,
                        int uv_stride, std::int64_t pts_ns, std::string& error);

/// Whether the encoder opened with NV12, which the GPU convert path requires.
bool encoder_is_nv12(Encoder& enc);

/// What the document says, in the terms the muxer needs.
///
/// The one place the two halves meet. `ldoc::Encode` carries what the operator chose -- a
/// codec, an implementation, a bitrate in kbps, an optional rescale -- and this turns it into
/// the pixel sizes, the bits per second and the tier name that `encoder_open` works in. Kept
/// here rather than in ldoc because ldoc knows nothing about FFmpeg and should not start.
///
/// `canvas` supplies the size when the encode has no rescale of its own, and the frame rate
/// always: a rescale changes how big the picture is, never how often it arrives.
[[nodiscard]] RuntimeConfig runtime_config(const ldoc::Encode& encode,
                                           const ldoc::AudioEncode& audio,
                                           const ldoc::Canvas& canvas, std::string path);

} // namespace libertas::encode
