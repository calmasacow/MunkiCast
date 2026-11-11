// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "llayout/geometry.hpp"

/// The Adobe drop-zone geometry of ADR-0016: a panel is cut by its own two diagonals, with a
/// rectangle carved out of the middle for the tabbed-group drop.
namespace llayout {

struct ZoneMetrics {
    /// Centre rectangle size as a proportion of each panel dimension.
    double centre_fraction = 0.34;
    /// Floor in physical pixels, so the tab drop stays hittable in a narrow panel.
    double centre_min_px = 56.0;
    /// Ceiling in physical pixels, so it does not swallow the canvas.
    double centre_max_px = 240.0;
    /// Hard ceiling as a proportion, so the four outer zones always have room to exist.
    double centre_max_fraction = 0.6;
};

/// The centre (tab) rectangle for `panel`, concentric with it.
[[nodiscard]] Rect centre_rect(const Rect& panel, const ZoneMetrics& m = {});

/// Which zone the point (px, py) falls in. Returns Zone::None if the point is outside the panel.
///
/// Implemented as "nearest edge", which is *exactly* the diagonal partition ADR-0016 specifies:
/// with u = x/w and v = y/h, the top zone is {v < u, v < 1-u}, and v < min(u, 1-u) implies
/// v < 0.5 = 1-v, so the nearest-edge test adds no constraint the diagonals did not already
/// impose. Nearest-edge is used instead of the four inequalities because it resolves the corners
/// -- where two zones meet at a single point -- deterministically rather than falling through to
/// whichever branch happened to be last.
///
/// Because u and v are normalised, the cut lines follow the panel's aspect ratio rather than
/// running at 45 degrees: a wide panel gets wide top and bottom zones. That is deliberate.
[[nodiscard]] Zone hit_test_zone(const Rect& panel, double px, double py,
                                 const ZoneMetrics& m = {});

/// The region a panel dropped into `z` would occupy, for the drag highlight. ADR-0016 rule 2
/// requires the highlight to preview the result, not show an abstract indicator.
[[nodiscard]] Rect zone_preview(const Rect& panel, Zone z);

} // namespace llayout
