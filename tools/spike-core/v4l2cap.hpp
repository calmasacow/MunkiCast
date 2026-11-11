// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace munkicast::spike {

struct V4l2Config {
    std::string device;
    std::uint32_t width = 0;      ///< 0 = keep the device's current setting
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;     ///< V4L2_PIX_FMT_*; 0 = keep current
    std::uint32_t fps_num = 0;    ///< 0 = do not request a rate
    std::uint32_t fps_den = 1;
    std::uint32_t frames = 120;
    bool export_dmabuf = false;   ///< use VIDIOC_EXPBUF to test the zero-copy path

    /// Colour signalling. These are REQUESTS: V4L2 lets a client state what it wants, and the
    /// driver is free to hand back something else, so what the device replies is what counts.
    /// 0 leaves the field at whatever the driver defaults to.
    std::uint32_t quantization = 0;  ///< V4L2_QUANTIZATION_FULL_RANGE / _LIM_RANGE
    std::uint32_t colorspace = 0;    ///< V4L2_COLORSPACE_*
    std::uint32_t ycbcr_enc = 0;     ///< V4L2_YCBCR_ENC_*
    std::uint32_t xfer_func = 0;     ///< V4L2_XFER_FUNC_*
};

struct V4l2Result {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    std::uint32_t captured = 0;
    /// What the driver actually replied with, which need not be what was asked for.
    std::uint32_t quantization = 0;
    std::uint32_t colorspace = 0;
    std::uint32_t ycbcr_enc = 0;
    std::uint32_t xfer_func = 0;
    bool dmabuf_exported = false;
    /// Intervals from the DRIVER's buffer timestamps, which are when the frame arrived at the
    /// hardware - not when our process got round to reading it.
    std::vector<double> driver_ms;
    std::vector<double> wall_ms;
    /// Per-frame statistics over the LUMA PLANE ONLY. A capture card with no input still delivers
    /// frames at full rate, and they are black, so "frames are flowing" is not evidence of signal.
    std::vector<double> luma;
    std::vector<std::uint8_t> luma_min;
    std::vector<std::uint8_t> luma_max;

    /// The DRIVER's frame counter. Inter-arrival time cannot see a frame the driver dropped
    /// before we ever dequeued it - the sequence number can, and it is the only instrument that
    /// distinguishes "the card is slow" from "the card is not receiving every source frame."
    std::uint32_t seq_first = 0;
    std::uint32_t seq_last = 0;
    std::uint32_t seq_breaks = 0;   ///< how many discontinuities
    std::uint64_t seq_missing = 0;  ///< total frames the driver skipped
    double span_ms = 0.0;           ///< driver-timestamp span, first frame to last
};

/// Captures from a V4L2 device and reports timing from the driver's own timestamps.
bool v4l2_capture(const V4l2Config& cfg, V4l2Result& out, std::string& error);

std::string v4l2_fourcc_str(std::uint32_t fourcc);
std::string v4l2_quantization_str(std::uint32_t q);
std::string v4l2_colorspace_str(std::uint32_t c);
std::string v4l2_ycbcr_enc_str(std::uint32_t e);
std::string v4l2_xfer_func_str(std::uint32_t x);
std::uint32_t v4l2_fourcc_parse(const std::string& s);

} // namespace munkicast::spike
