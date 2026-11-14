// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <linux/videodev2.h>
#include <numeric>
#include <optional>
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

/// Display luminance, in nits, for one ST 2084 (PQ) code value in 0..1.
///
/// The inverse EOTF is what makes a PQ capture readable at all: the code values are perceptually
/// spaced over 0-10000 nits, so reading them as if they were Rec.709 gamma gives the flat, dark
/// picture a 4K X produces today -- a measured HDR loop came back 27-154 of 255 where the card's
/// own 8-bit conversion of the same signal gave 0-230.
[[nodiscard]] inline double pq_nits(double code) {
    constexpr double m1 = 0.1593017578125, m2 = 78.84375;
    constexpr double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    if (!(code > 0.0))
        return 0.0;
    const double v = std::pow(std::min(code, 1.0), 1.0 / m2);
    const double numerator = std::max(v - c1, 0.0);
    const double denominator = c2 - c3 * v;
    if (!(denominator > 0.0))
        return 10000.0;
    return 10000.0 * std::pow(numerator / denominator, 1.0 / m1);
}

/// The Rec.709 / sRGB-style OETF: linear light in 0..1 to a display signal in 0..1.
///
/// Needed because PQ decodes to *luminance*, and the BGRA a capture delivers is a display signal
/// that something further down will treat as gamma-encoded. Measured without it: the tone-mapped
/// frame put 50% of its pixels in the bottom eighth of the range against the card's own 14.6%,
/// and the mean luma came out 50 where the card's 8-bit conversion of the same signal gave 72.
/// Dark and bottom-heavy, which is a missing OETF rather than a wrong roll off.
[[nodiscard]] inline double linear_to_display(double linear) {
    const double v = std::clamp(linear, 0.0, 1.0);
    return v <= 0.018053968510807 ? 4.5 * v : 1.09929682680944 * std::pow(v, 0.45) - 0.09929682680944;
}

/// A PQ code value mapped to an SDR display signal in 0..1.
///
/// Delivery is SDR (ADR-0071), so the question is not whether to tone-map but where the roll off
/// goes. The curve is Reinhard with a white point across the whole range: it is monotonic, it
/// passes through zero, and it sends `peak` nits to 1.0 instead of clipping there.
///
/// Deliberately *not* linear-below-reference with a shoulder above. That was tried and is wrong:
/// it makes the function non-monotonic, so a brighter input comes out darker -- 100 nits reached
/// 0.999 while 201 nits fell back to 0.682, which on a real frame means highlights turning into
/// dark patches. One curve everywhere cannot do that.
///
/// `reference` is the nits that should land at mid-grey-to-white, 100 because that is SDR white;
/// `peak` is the brightest the source is assumed to go, mapped to 1.0. A real HDR loop measured a
/// 98-nit median and a 201-nit peak, so ordinary content sits in the upper middle of the range
/// and the genuine highlights have somewhere to go.
[[nodiscard]] inline double pq_to_sdr(double code, double reference = 100.0,
                                      double peak = 1000.0) {
    if (!(reference > 0.0))
        return 0.0;
    const double scaled = pq_nits(code) / reference;
    if (!(scaled > 0.0))
        return 0.0;
    const double white = std::max(peak / reference, 1.0 + 1e-6);
    // x(1 + x/w^2)/(1 + x): strictly increasing in x for w > 1, 0 at 0, and 1 at x == w. This is
    // still linear light, so it goes through the OETF before it can be a display signal.
    const double mapped = scaled * (1.0 + scaled / (white * white)) / (1.0 + scaled);
    return linear_to_display(mapped);
}

/// Whether a 10-bit plane carries real 10-bit data rather than 8-bit values padded into it.
///
/// Needed because the bit depth of the incoming signal cannot be asked for. On the Elgato 4K X
/// (the only HDR-capable card tested) `VIDIOC_QUERY_DV_TIMINGS` returns ENOTTY, and P010 is
/// enumerated from a static USB descriptor at every size whatever is plugged in -- so the
/// capability says nothing about the signal. What does say something is the data: a genuinely
/// 10-bit frame scatters its low two bits, while 8-bit values widened into a 10-bit word leave
/// them all zero.
///
/// `words` is the 16-bit luma plane, P010-style: the ten significant bits sit at the top, so the
/// low six are the padding and the low two of the *value* are `(word >> 6) & 3`. Sampled rather
/// than scanned -- 8.3 million pixels of 4K is more than the answer needs. A real HDR frame
/// measured 76% of pixels with those bits set.
///
/// Returns nothing for a frame with no picture in it. This is not a nicety: a 4K X delivers two
/// blank frames before the first real one, and a black frame's low bits are all zero in either
/// depth, so deciding on it answers "8-bit" for every signal. Measured -- the first version of
/// this did exactly that on a live PQ feed. The caller keeps asking until a frame has content.
[[nodiscard]] inline std::optional<bool> plane_is_truly_ten_bit(const std::uint16_t* words,
                                                                std::size_t count) {
    if (words == nullptr || count == 0)
        return std::nullopt;
    const std::size_t step = count > 4096 ? count / 4096 : 1;
    std::size_t looked = 0, scattered = 0;
    std::uint16_t lowest = 0xffffu, highest = 0;
    for (std::size_t i = 0; i < count; i += step, ++looked) {
        const auto value = static_cast<std::uint16_t>(words[i] >> 6);
        if ((value & 3u) != 0)
            ++scattered;
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    if (looked == 0)
        return std::nullopt;
    // A flat frame says nothing about depth whatever its level: black, or a test card's grey.
    // Four code values of spread is well inside any real picture and well outside a blank one.
    if (highest - lowest < 4)
        return std::nullopt;
    // A twentieth is far below the 76% a real frame showed and far above the zero that padding
    // gives. Dithered 8-bit content is the case this is deliberately generous about: better to
    // treat a doubtful frame as 8-bit than to tone-map an SDR signal that never asked for it.
    return scattered * 20 >= looked;
}

inline bool matches_signal_rate(const SignalMode& signal, std::uint32_t numerator,
                                std::uint32_t denominator) {
    return numerator && denominator &&
        std::abs(static_cast<double>(numerator) / denominator - signal.fps()) <= signal.fps() * 0.005;
}
} // namespace munkicast::capture::detail
