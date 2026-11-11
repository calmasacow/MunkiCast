// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/dropzone.hpp"

#include <algorithm>

namespace llayout {
namespace {

/// One dimension of the centre rectangle: a proportion, clamped into physical bounds, then
/// capped proportionally so the outer zones cannot be squeezed out of existence.
double centre_extent(double panel_extent, const ZoneMetrics& m) {
    const double want = std::clamp(panel_extent * m.centre_fraction, m.centre_min_px,
                                   m.centre_max_px);
    return std::min(want, panel_extent * m.centre_max_fraction);
}

} // namespace

Rect centre_rect(const Rect& panel, const ZoneMetrics& m) {
    const double cw = centre_extent(panel.w, m);
    const double ch = centre_extent(panel.h, m);
    return Rect{panel.x + (panel.w - cw) * 0.5, panel.y + (panel.h - ch) * 0.5, cw, ch};
}

Zone hit_test_zone(const Rect& panel, double px, double py, const ZoneMetrics& m) {
    if (!panel.contains(px, py))
        return Zone::None;

    if (centre_rect(panel, m).contains(px, py))
        return Zone::Center;

    // Normalised distances to each edge. The smallest wins; see the header for why this is the
    // diagonal partition.
    const double u = (panel.w > 0.0) ? (px - panel.x) / panel.w : 0.5;
    const double v = (panel.h > 0.0) ? (py - panel.y) / panel.h : 0.5;

    const double d_left = u;
    const double d_right = 1.0 - u;
    const double d_top = v;
    const double d_bottom = 1.0 - v;

    // Tie-break order is horizontal first. At an exact corner both zones are equally defensible;
    // side docking is the more common gesture in a video application, which wants tall columns,
    // so a corner resolves to Left or Right.
    double best = d_left;
    Zone zone = Zone::Left;
    if (d_right < best) {
        best = d_right;
        zone = Zone::Right;
    }
    if (d_top < best) {
        best = d_top;
        zone = Zone::Top;
    }
    if (d_bottom < best) {
        zone = Zone::Bottom;
    }
    return zone;
}

Rect zone_preview(const Rect& panel, Zone z) {
    switch (z) {
    case Zone::Left:   return Rect{panel.x, panel.y, panel.w * 0.5, panel.h};
    case Zone::Right:  return Rect{panel.x + panel.w * 0.5, panel.y, panel.w * 0.5, panel.h};
    case Zone::Top:    return Rect{panel.x, panel.y, panel.w, panel.h * 0.5};
    case Zone::Bottom: return Rect{panel.x, panel.y + panel.h * 0.5, panel.w, panel.h * 0.5};
    case Zone::Center: return panel;
    case Zone::None:   break;
    }
    return Rect{};
}

} // namespace llayout
