// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lcore/ulid.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <variant>

namespace ldoc {

/// Every addressable thing in a project carries one of these, and every reference is by id.
using Id = lc::Ulid;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Vec2&, const Vec2&) = default;
};

/// A colour in the document.
///
/// **Scene-linear, premultiplied, not sRGB-encoded.** The compositor works in linear light
/// (RGBA16F), so a colour stored with the sRGB transfer function still applied would be wrong
/// everywhere it was blended -- subtly, in a way that looks like "the stroke is too dark" rather
/// than like a bug. The UI picks colours in sRGB and converts at the boundary; `from_srgb8` and
/// `to_srgb8` are that boundary and the only place the transfer function is applied.
struct Rgba {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;

    [[nodiscard]] static Rgba from_srgb8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                         std::uint8_t a = 255);
    /// Returns the four 8-bit sRGB components, rounded.
    [[nodiscard]] std::array<std::uint8_t, 4> to_srgb8() const;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

/// A value in an effect's parameter set or a source's settings.
///
/// `std::monostate` is an explicitly stored null, not an absent key. The distinction matters
/// because the project format serialises *all* defaults rather than omitting them: OBS's sparse
/// settings blobs are exactly why its own importer has to carry a per-source-id default table,
/// and why a plugin upgrade can silently change what an unset field means.
using ParamValue =
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Vec2, Rgba, Id>;

using ParamMap = std::map<std::string, ParamValue, std::less<>>;

/// A frame rate as an exact rational. Never a double: 59.94 is 60000/1001 and nothing else, and
/// a pipeline that rounds it accumulates drift that shows up hours into a broadcast as audio
/// sync. The reference rig runs exactly this rate (ADR-0013).
struct Fps {
    std::int32_t num = 60;
    std::int32_t den = 1;

    [[nodiscard]] double hz() const;
    [[nodiscard]] bool valid() const { return num > 0 && den > 0; }
    [[nodiscard]] static constexpr Fps ntsc(std::int32_t whole) {
        return Fps{whole * 1000, 1001};
    }

    friend bool operator==(const Fps&, const Fps&) = default;
};

enum class ColorSpace : std::uint8_t {
    Srgb,
    Rec709,
    Rec2020,
    Rec2100Pq,
    Rec2100Hlg,
};

[[nodiscard]] std::string_view to_string(ColorSpace cs);
[[nodiscard]] std::optional<ColorSpace> color_space_from_string(std::string_view s);

/// How a source's pixels are fitted into the node's box, in *source* space.
///
/// Kept separate from Transform on purpose. Conflating "where this sits on the canvas" with "how
/// the image fills its box" is why OBS bounding boxes confuse people: scaling a source and
/// changing its fit mode look like the same operation until they disagree.
enum class FitMode : std::uint8_t {
    /// The box takes the source's natural size; no fitting happens.
    Natural,
    /// Fill the box exactly, changing aspect ratio.
    Stretch,
    /// Largest fit that shows the whole source, letterboxing the remainder.
    Contain,
    /// Smallest fit that fills the box, cropping the overflow.
    Cover,
    FillWidth,
    FillHeight,
};

[[nodiscard]] std::string_view to_string(FitMode m);
[[nodiscard]] std::optional<FitMode> fit_mode_from_string(std::string_view s);

/// A fraction of `space` reported back as pixels, rounded to a hundredth of one (ADR-0070).
///
/// The stored value is not rounded -- it is the authored geometry and stays exact. This is for
/// the way out, where a round number of pixels need not survive the trip: 123 of a 1920 canvas
/// is 123/1920, which is not representable in binary, and comes back as 122.99999999999999.
/// Nothing is placed to finer than a hundredth of a pixel, so a reader asking in pixels gets
/// the number the operator typed rather than that.
[[nodiscard]] inline double in_pixels(double fraction, double space) {
    return std::round(fraction * space * 100.0) / 100.0;
}

/// Crop in source pixels, taken off each edge before fitting.
///
/// Source pixels, not canvas fractions: a crop is a statement about the incoming image and has
/// nothing to do with the canvas it is drawn onto (ADR-0070).
struct CropRect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    [[nodiscard]] bool any() const { return left != 0.0 || top != 0.0 || right != 0.0 || bottom != 0.0; }
    friend bool operator==(const CropRect&, const CropRect&) = default;
};

/// Where the node sits in its containing space.
///
/// `position` is a **fraction** of the space the node is composited into -- the canvas for a
/// root node, the group's or precomp's own surface for a nested one. `{0.5, 0.5}` is half way
/// across at any resolution, which is what lets a 1080p show reopen at 4K without every source
/// moving (ADR-0070). The UI still shows and accepts pixels, computed against the current
/// canvas; the fraction is the storage format, not the interface.
struct Transform {
    Vec2 position{};
    double rotation_deg = 0.0;
    Vec2 scale{1.0, 1.0};
    /// Normalised within the node's own box; the point rotation and scale act around.
    Vec2 pivot{0.5, 0.5};
    Vec2 skew_deg{};

    friend bool operator==(const Transform&, const Transform&) = default;
};

/// How the content fills the node's box.
struct Layout {
    FitMode fit = FitMode::Natural;
    CropRect crop{};
    /// The box, as a **fraction** of the containing space (ADR-0070). (0, 0) still means
    /// "the source's natural size", which is resolution-independent already because it is a
    /// property of the source rather than of the canvas.
    Vec2 size{};
    /// Where the fitted content sits inside the box when it does not fill it.
    Vec2 alignment{0.5, 0.5};

    friend bool operator==(const Layout&, const Layout&) = default;
};

enum class BlendMode : std::uint8_t {
    Normal,
    Dissolve,
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    Lighten,
    Screen,
    ColorDodge,
    LinearDodge,
    Overlay,
    SoftLight,
    HardLight,
    VividLight,
    LinearLight,
    PinLight,
    HardMix,
    Difference,
    Exclusion,
    Subtract,
    Divide,
    Hue,
    Saturation,
    Color,
    Luminosity,
};

/// True for the four modes that cannot be computed per channel and therefore need the backdrop.
///
/// This is not a rendering detail that can be discovered later. A subtree containing one of these
/// has to be flattened to an intermediate before it can be composited, so the render graph needs
/// a group-flattening pass planner from day one; retrofitting it is a compositor rewrite.
[[nodiscard]] bool needs_backdrop(BlendMode mode);

[[nodiscard]] std::string_view to_string(BlendMode m);
[[nodiscard]] std::optional<BlendMode> blend_mode_from_string(std::string_view s);

struct Blend {
    BlendMode mode = BlendMode::Normal;
    double opacity = 1.0;
    /// Composite only where the node beneath it has alpha.
    bool clip_to_below = false;
    /// For groups: blend children directly against the group's backdrop rather than flattening
    /// the group first. Changes the result for every mode except Normal at full opacity.
    bool pass_through_group = false;

    friend bool operator==(const Blend&, const Blend&) = default;
};

} // namespace ldoc
