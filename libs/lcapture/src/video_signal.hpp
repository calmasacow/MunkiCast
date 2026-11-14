// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <linux/videodev2.h>
#include <numeric>
#include <string>

namespace munkicast::capture::detail {
struct SignalMode {
    int width = 0, height = 0;
    std::uint32_t numerator = 0, denominator = 1;
    double fps() const { return static_cast<double>(numerator) / denominator; }
};

inline std::expected<SignalMode, std::string> signal_mode(const v4l2_dv_timings& timings) {
    if (timings.type != V4L2_DV_BT_656_1120)
        return std::unexpected("Unsupported incoming signal timing type.");
    const auto& bt = timings.bt;
    if (bt.interlaced)
        return std::unexpected("Interlaced incoming signals are not supported.");
    const std::uint64_t width = std::uint64_t{bt.width} + bt.hfrontporch + bt.hsync + bt.hbackporch;
    const std::uint64_t height = std::uint64_t{bt.height} + bt.vfrontporch + bt.vsync + bt.vbackporch;
    if (!bt.width || !bt.height || bt.width > 8192 || bt.height > 8192 ||
        bt.width % 2 || bt.height % 2 || width > 65536 || height > 65536 || !bt.pixelclock)
        return std::unexpected("Invalid incoming signal dimensions or pixel clock.");
    const double fps = static_cast<double>(bt.pixelclock) / static_cast<double>(width * height);
    if (fps < 1 || fps > 240)
        return std::unexpected("Incoming signal frame rate is outside the supported 1–240 FPS range.");
    // Pixel clocks may be rounded by the receiver. Retain micro-FPS precision, then negotiate
    // an advertised USB interval instead of assuming its clock has the same exact fraction.
    const auto numerator = static_cast<std::uint32_t>(std::llround(fps * 1000000));
    const auto divisor = std::gcd(numerator, 1000000u);
    return SignalMode{static_cast<int>(bt.width), static_cast<int>(bt.height),
                      numerator / divisor, 1000000u / divisor};
}

/// What a captured signal's colour is, from the format V4L2 negotiated.
///
/// Classified in one place because the three cases need different handling and are easy to
/// confuse: Rec.2100 HLG has no transfer code of its own -- it is BT.2020 primaries with the
/// HLG curve -- so it cannot be told from wide-gamut SDR by `colorspace` alone (ADR-0071).
struct SignalColour {
    /// SMPTE ST 2084. Highlights run far above SDR white.
    bool pq = false;
    /// BT.2020 primaries with no stated curve, which is how HLG arrives.
    bool hlg = false;
    /// BT.2020 primaries, whatever the curve. Needs the 2020 matrix to decode without a hue
    /// shift, which is true of the SDR case as well as the two HDR ones.
    bool wide = false;

    [[nodiscard]] bool hdr() const { return pq || hlg; }
};

inline SignalColour signal_colour(std::uint32_t colorspace, std::uint32_t xfer_func) {
    SignalColour out;
    out.wide = colorspace == V4L2_COLORSPACE_BT2020;
    out.pq = xfer_func == V4L2_XFER_FUNC_SMPTE2084;
    // Only when nothing else claimed the curve: a card that states both BT.2020 and ST 2084 is
    // PQ, not HLG, and treating it as both would log the wrong one.
    out.hlg = out.wide && !out.pq && xfer_func == V4L2_XFER_FUNC_NONE;
    return out;
}

inline bool matches_signal_rate(const SignalMode& signal, std::uint32_t numerator,
                                std::uint32_t denominator) {
    return numerator && denominator &&
        std::abs(static_cast<double>(numerator) / denominator - signal.fps()) <= signal.fps() * 0.005;
}
} // namespace munkicast::capture::detail
