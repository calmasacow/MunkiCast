// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

/// Layout geometry primitives.
///
/// Everything here is `double`. Not because a panel needs sub-nanometre precision, but because
/// mixing `float` and `double` under -Wdouble-promotion is a running paper cut, and Qt's `qreal`
/// is a double anyway, so one type crosses the whole UI with no conversions to audit.
namespace llayout {

/// The direction children of a split are laid out in.
/// Horizontal: children sit side by side, dividers are vertical.
enum class Axis : std::uint8_t { Horizontal, Vertical };

/// One of the five drop zones of ADR-0016, or none.
enum class Zone : std::uint8_t { None, Left, Right, Top, Bottom, Center };

/// An outer edge of the whole workspace (ADR-0016 rule 6).
enum class Edge : std::uint8_t { Left, Right, Top, Bottom };

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;

    [[nodiscard]] constexpr double right() const { return x + w; }
    [[nodiscard]] constexpr double bottom() const { return y + h; }
    [[nodiscard]] constexpr bool empty() const { return w <= 0.0 || h <= 0.0; }

    /// Half-open on the far edges, so adjacent rects tile without overlap or gaps.
    [[nodiscard]] constexpr bool contains(double px, double py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

[[nodiscard]] constexpr double extent_along(const Rect& r, Axis a) {
    return a == Axis::Horizontal ? r.w : r.h;
}

[[nodiscard]] constexpr double origin_along(const Rect& r, Axis a) {
    return a == Axis::Horizontal ? r.x : r.y;
}

/// The axis a drop into `z` splits along. Center does not split, so it has no axis.
[[nodiscard]] constexpr Axis axis_of(Zone z) {
    return (z == Zone::Left || z == Zone::Right) ? Axis::Horizontal : Axis::Vertical;
}

[[nodiscard]] constexpr Axis axis_of(Edge e) {
    return (e == Edge::Left || e == Edge::Right) ? Axis::Horizontal : Axis::Vertical;
}

/// True when a drop into `z` puts the new panel before the target along the axis.
[[nodiscard]] constexpr bool is_leading(Zone z) { return z == Zone::Left || z == Zone::Top; }
[[nodiscard]] constexpr bool is_leading(Edge e) { return e == Edge::Left || e == Edge::Top; }

[[nodiscard]] constexpr const char* to_string(Axis a) {
    return a == Axis::Horizontal ? "h" : "v";
}

[[nodiscard]] constexpr const char* to_string(Zone z) {
    switch (z) {
    case Zone::None:   return "none";
    case Zone::Left:   return "left";
    case Zone::Right:  return "right";
    case Zone::Top:    return "top";
    case Zone::Bottom: return "bottom";
    case Zone::Center: return "center";
    }
    return "none";
}

[[nodiscard]] constexpr const char* to_string(Edge e) {
    switch (e) {
    case Edge::Left:   return "left";
    case Edge::Right:  return "right";
    case Edge::Top:    return "top";
    case Edge::Bottom: return "bottom";
    }
    return "left";
}

} // namespace llayout
