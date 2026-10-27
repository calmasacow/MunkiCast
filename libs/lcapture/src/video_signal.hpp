// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <expected>
#include <linux/videodev2.h>
#include <numeric>
#include <string>

namespace libertas::capture::detail {
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

inline bool matches_signal_rate(const SignalMode& signal, std::uint32_t numerator,
                                std::uint32_t denominator) {
    return numerator && denominator &&
        std::abs(static_cast<double>(numerator) / denominator - signal.fps()) <= signal.fps() * 0.005;
}
} // namespace libertas::capture::detail
