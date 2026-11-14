// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace munkicast::capture {
struct VideoDevice {
    std::string path, name;
    int width = 0, height = 0;
};
std::vector<VideoDevice> list_video_devices();
struct VideoFormat { std::string code, name; };
std::vector<VideoFormat> list_video_formats(const std::string& path);
struct VideoOptions {
    std::string format; // Empty or Auto selects a supported format.
    bool buffering = true;
    bool auto_reset = false;
    int timeout_frames = 5;
    /// Deliver YUYV/I420/NV12 untouched for the GPU to convert, instead of BGRA from swscale.
    bool raw_yuv = false;
    /// Prefer a 10-bit capture format, and tone-map a PQ signal to SDR.
    ///
    /// Operator-set rather than detected, because the depth of the incoming signal cannot be
    /// asked for: the one HDR card tested answers VIDIOC_QUERY_DV_TIMINGS with ENOTTY and
    /// enumerates P010 from a static USB descriptor whatever is plugged in (issue #20). Asking
    /// the card and then checking a frame would mean reopening every ordinary camera that turned
    /// out to be 8-bit, so this is a setting instead. A card with no 10-bit format ignores it,
    /// and a 10-bit format carrying 8-bit values is left alone rather than tone-mapped.
    bool ten_bit = false;
    friend bool operator==(const VideoOptions&, const VideoOptions&) = default;
};
/// Raw camera layouts the GPU compositor converts itself. Bgra is already converted.
enum class VideoLayout : std::uint8_t { Bgra, Yuyv, I420, Nv12 };
struct VideoFrame {
    int width = 0, height = 0;
    /// Driver sequence, including frames discarded by the low-latency dequeue path.
    std::uint32_t sequence = 0;
    /// V4L2 monotonic capture time; empty when the driver supplies another clock.
    std::chrono::steady_clock::time_point captured_at{};
    /// Tightly packed BGRA8, or for a YUV layout the tightly packed planes (Y, then U/V or UV).
    std::vector<std::uint8_t> pixels;
    VideoLayout layout = VideoLayout::Bgra;
    bool bt709 = false, full_range = false;
};
class VideoCapture {
  public:
    using Callback = std::function<void(std::shared_ptr<const VideoFrame>)>;
    static std::unique_ptr<VideoCapture> open(const std::string& path,
        const std::string& resolution, const std::string& rate, const std::string& range,
        std::string& error, const VideoOptions& options = {});
    ~VideoCapture();
    void start(Callback callback);
    void stop();
    [[nodiscard]] double negotiated_fps() const;
    /// Poll incoming timing at most once per second for automatic modes. Explicit modes stay fixed.
    [[nodiscard]] bool signal_changed();
  private:
    struct State;
    explicit VideoCapture(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace munkicast::capture
