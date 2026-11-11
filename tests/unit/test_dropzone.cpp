// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The drop-zone geometry of ADR-0016.

#include "llayout/dropzone.hpp"

#include "ltest.hpp"

#include <utility>

using namespace llayout;

namespace {

const Rect kPanel{100.0, 50.0, 800.0, 600.0};

} // namespace

LTEST_CASE(centre_rect_is_concentric_and_inside) {
    const Rect c = centre_rect(kPanel);
    CHECK_NEAR(c.x + c.w * 0.5, kPanel.x + kPanel.w * 0.5, 1e-9);
    CHECK_NEAR(c.y + c.h * 0.5, kPanel.y + kPanel.h * 0.5, 1e-9);
    CHECK(c.x >= kPanel.x);
    CHECK(c.y >= kPanel.y);
    CHECK(c.right() <= kPanel.right());
    CHECK(c.bottom() <= kPanel.bottom());
}

LTEST_CASE(centre_rect_respects_the_physical_floor_and_ceiling) {
    const ZoneMetrics m{};

    // Tiny panel: the proportion would give a few pixels, so the floor takes over -- but the
    // proportional ceiling still leaves room for the outer zones to exist.
    const Rect tiny{0.0, 0.0, 90.0, 70.0};
    const Rect ct = centre_rect(tiny, m);
    CHECK(ct.w <= tiny.w * m.centre_max_fraction + 1e-9);
    CHECK(ct.h <= tiny.h * m.centre_max_fraction + 1e-9);
    CHECK(ct.w > 0.0);

    // Huge panel: the proportion would swallow the canvas, so the pixel ceiling takes over.
    const Rect huge{0.0, 0.0, 3840.0, 2160.0};
    const Rect ch = centre_rect(huge, m);
    CHECK_NEAR(ch.w, m.centre_max_px, 1e-9);
    CHECK_NEAR(ch.h, m.centre_max_px, 1e-9);
}

LTEST_CASE(the_five_zones_at_canonical_points) {
    const double cx = kPanel.x + kPanel.w * 0.5;
    const double cy = kPanel.y + kPanel.h * 0.5;

    CHECK_EQ(hit_test_zone(kPanel, cx, cy), Zone::Center);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.x + 4.0, cy), Zone::Left);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.right() - 4.0, cy), Zone::Right);
    CHECK_EQ(hit_test_zone(kPanel, cx, kPanel.y + 4.0), Zone::Top);
    CHECK_EQ(hit_test_zone(kPanel, cx, kPanel.bottom() - 4.0), Zone::Bottom);
}

LTEST_CASE(a_point_outside_the_panel_is_no_zone) {
    CHECK_EQ(hit_test_zone(kPanel, 0.0, 0.0), Zone::None);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.right(), kPanel.y), Zone::None);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.x, kPanel.bottom()), Zone::None);
}

LTEST_CASE(corners_resolve_deterministically_and_never_to_none) {
    // A corner is where two zones meet at a point, so no answer there is "right"; what matters is
    // that there is always an answer and it is always the same one. kPanel is landscape, so half a
    // pixel in from a corner the *normalised* distance to the near vertical edge is the smaller
    // one, and the horizontal zone wins.
    CHECK_EQ(hit_test_zone(kPanel, kPanel.x + 0.5, kPanel.y + 0.5), Zone::Left);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.right() - 0.5, kPanel.y + 0.5), Zone::Right);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.x + 0.5, kPanel.bottom() - 0.5), Zone::Left);
    CHECK_EQ(hit_test_zone(kPanel, kPanel.right() - 0.5, kPanel.bottom() - 0.5), Zone::Right);

    // In a portrait panel the same gesture resolves vertically, for the same reason.
    const Rect portrait{0.0, 0.0, 600.0, 800.0};
    CHECK_EQ(hit_test_zone(portrait, 0.5, 0.5), Zone::Top);
    CHECK_EQ(hit_test_zone(portrait, 599.5, 799.5), Zone::Bottom);

    // At the exact top-left vertex both distances are zero, and the documented tie-break --
    // horizontal first -- decides it.
    CHECK_EQ(hit_test_zone(kPanel, kPanel.x, kPanel.y), Zone::Left);

    // Whatever each corner answers, it never answers None and never changes its mind.
    for (const auto& [x, y] : {std::pair{kPanel.x, kPanel.y},
                               std::pair{kPanel.right() - 1e-7, kPanel.y},
                               std::pair{kPanel.x, kPanel.bottom() - 1e-7},
                               std::pair{kPanel.right() - 1e-7, kPanel.bottom() - 1e-7}}) {
        const Zone z = hit_test_zone(kPanel, x, y);
        CHECK(z != Zone::None);
        CHECK_EQ(hit_test_zone(kPanel, x, y), z);
    }
}

LTEST_CASE(the_cut_lines_follow_the_aspect_ratio_not_45_degrees) {
    // One physical point, 100 px right and 60 px down from the top-left corner, in two panels of
    // different shape. Cuts at 45 degrees would answer Top in both cases, since 60 < 100. Because
    // u and v are normalised instead, the cut follows the panel's own diagonal:
    //
    //   wide panel  (1000x200): 60 px down is 30% of the height but 100 px across is only 10% of
    //                           the width, so the point is nearer the left edge  -> Left
    //   tall panel  (200x1000): the same two offsets swap roles                  -> Top
    //
    // Which is ADR-0016's point: a wide panel gets top and bottom zones that are wide but
    // shallow, and the gesture stays forgiving because every point belongs to the zone it is
    // nearest to in the direction that matters.
    const Rect wide{0.0, 0.0, 1000.0, 200.0};
    const Rect tall{0.0, 0.0, 200.0, 1000.0};
    CHECK_EQ(hit_test_zone(wide, 100.0, 60.0), Zone::Left);
    CHECK_EQ(hit_test_zone(tall, 60.0, 100.0), Zone::Top);

    // And the zones really are wide: most of the width of a wide panel, sampled near the top,
    // is Top.
    int top = 0;
    for (double x = 5.0; x < 1000.0; x += 10.0)
        top += hit_test_zone(wide, x, 3.0) == Zone::Top ? 1 : 0;
    CHECK(top > 90);
}

LTEST_CASE(nearest_edge_is_exactly_the_partition_the_adr_specifies) {
    // ADR-0016 writes the outer zones as four inequalities on u and v. The implementation uses
    // nearest-edge instead, because it resolves the corners. This sweeps the panel and asserts
    // the two formulations agree everywhere they are both defined -- that is, everywhere except
    // exactly on a diagonal, where the ADR's strict inequalities name no zone at all.
    const ZoneMetrics m{};
    const Rect c = centre_rect(kPanel, m);
    int compared = 0;

    for (double y = kPanel.y + 0.5; y < kPanel.bottom(); y += 3.0) {
        for (double x = kPanel.x + 0.5; x < kPanel.right(); x += 3.0) {
            const Zone got = hit_test_zone(kPanel, x, y, m);
            REQUIRE(got != Zone::None);
            if (c.contains(x, y)) {
                CHECK_EQ(got, Zone::Center);
                continue;
            }
            const double u = (x - kPanel.x) / kPanel.w;
            const double v = (y - kPanel.y) / kPanel.h;

            const bool adr_top = v < u && v < 1.0 - u;
            const bool adr_bottom = v > u && v > 1.0 - u;
            const bool adr_left = v > u && v < 1.0 - u;
            const bool adr_right = v < u && v > 1.0 - u;
            const int named = int(adr_top) + int(adr_bottom) + int(adr_left) + int(adr_right);
            if (named != 1)
                continue; // on a diagonal: the ADR's inequalities are silent here

            ++compared;
            const Zone want = adr_top      ? Zone::Top
                              : adr_bottom ? Zone::Bottom
                              : adr_left   ? Zone::Left
                                           : Zone::Right;
            CHECK_MSG(got == want, "at u={:.4f} v={:.4f}: got {} want {}", u, v, to_string(got),
                      to_string(want));
        }
    }
    CHECK(compared > 10000);
}

LTEST_CASE(the_partition_is_mirror_symmetric) {
    const double cx = kPanel.x + kPanel.w * 0.5;
    const double cy = kPanel.y + kPanel.h * 0.5;
    auto flip = [](Zone z, bool horizontal) {
        if (horizontal)
            return z == Zone::Left ? Zone::Right : z == Zone::Right ? Zone::Left : z;
        return z == Zone::Top ? Zone::Bottom : z == Zone::Bottom ? Zone::Top : z;
    };

    for (double y = kPanel.y + 0.5; y < kPanel.bottom(); y += 7.0) {
        for (double x = kPanel.x + 0.5; x < kPanel.right(); x += 7.0) {
            const Zone z = hit_test_zone(kPanel, x, y);
            const double mx = 2.0 * cx - x;
            const double my = 2.0 * cy - y;
            if (kPanel.contains(mx, y))
                CHECK_EQ(hit_test_zone(kPanel, mx, y), flip(z, true));
            if (kPanel.contains(x, my))
                CHECK_EQ(hit_test_zone(kPanel, x, my), flip(z, false));
        }
    }
}

LTEST_CASE(the_preview_shows_the_resulting_region) {
    CHECK_NEAR(zone_preview(kPanel, Zone::Left).w, kPanel.w * 0.5, 1e-9);
    CHECK_NEAR(zone_preview(kPanel, Zone::Left).h, kPanel.h, 1e-9);
    CHECK_NEAR(zone_preview(kPanel, Zone::Right).x, kPanel.x + kPanel.w * 0.5, 1e-9);
    CHECK_NEAR(zone_preview(kPanel, Zone::Bottom).y, kPanel.y + kPanel.h * 0.5, 1e-9);
    CHECK_NEAR(zone_preview(kPanel, Zone::Top).h, kPanel.h * 0.5, 1e-9);

    // A tab drop takes the target's exact space (ADR-0016 rule 4).
    const Rect c = zone_preview(kPanel, Zone::Center);
    CHECK_NEAR(c.x, kPanel.x, 1e-9);
    CHECK_NEAR(c.y, kPanel.y, 1e-9);
    CHECK_NEAR(c.w, kPanel.w, 1e-9);
    CHECK_NEAR(c.h, kPanel.h, 1e-9);

    CHECK(zone_preview(kPanel, Zone::None).empty());
}

LTEST_MAIN("dropzone")
