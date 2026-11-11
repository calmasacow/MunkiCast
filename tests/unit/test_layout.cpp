// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layout tree of ADR-0015 and the docking gestures of ADR-0016.

#include "llayout/layout.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <random>
#include <set>

using namespace llayout;

namespace {

Viewport vp_of(double w, double h) {
    Viewport vp;
    vp.bounds = Rect{0.0, 0.0, w, h};
    return vp;
}

/// A viewport with no divider or tab-bar thickness, so proportions are the only thing in play.
Viewport proportional_vp(double w, double h) {
    Viewport vp = vp_of(w, h);
    vp.divider_px = 0.0;
    vp.tab_bar_px = 0.0;
    return vp;
}

std::size_t depth(const Layout& l, NodeId id) {
    const Node* n = l.find(id);
    if (!n || !n->is_split())
        return 1;
    std::size_t d = 0;
    for (const NodeId c : n->children)
        d = std::max(d, depth(l, c));
    return d + 1;
}

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

std::vector<std::string> sorted_panels(const Layout& l) {
    std::vector<std::string> p = l.panels();
    std::sort(p.begin(), p.end());
    return p;
}

} // namespace

LTEST_CASE(the_default_layout_is_valid_and_holds_the_expected_panels) {
    const Layout l = Layout::default_studio();
    const auto bad = l.validate();
    CHECK_MSG(!bad.has_value(), "{}", bad.value_or(""));

    CHECK_EQ(sorted_panels(l),
             (std::vector<std::string>{"audio-mixer", "broadcast-stats", "controls", "countdowns",
                                       "favourites", "info", "log", "outputs", "program",
                                       "properties", "scenes", "sources"}));

    // The program canvas is a region of the workspace, not a window (ADR-0015 rule 2).
    CHECK(l.has_panel("program"));
    CHECK(l.node_of_panel("program") != kInvalidNode);

    // properties, info and log ship as one tabbed group, so the tab path is exercised by the
    // default layout.
    CHECK_EQ(l.node_of_panel("info"), l.node_of_panel("log"));
    CHECK_EQ(l.node_of_panel("properties"), l.node_of_panel("log"));
}

LTEST_CASE(an_empty_layout_is_valid) {
    const Layout l;
    CHECK(!l.validate().has_value());
    CHECK(l.panels().empty());
    CHECK_EQ(l.node_count(), std::size_t{1});
}

LTEST_CASE(leaves_tile_the_viewport_exactly) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = proportional_vp(2560.0, 1440.0);

    const std::vector<LeafRect> lf = l.leaves(vp, cs);
    REQUIRE(lf.size() == 8); // eight groups hold the twelve panels

    double area = 0.0;
    for (const LeafRect& a : lf) {
        CHECK(a.rect.w > 0.0);
        CHECK(a.rect.h > 0.0);
        area += a.rect.w * a.rect.h;
        CHECK(a.rect.x >= -1e-9);
        CHECK(a.rect.y >= -1e-9);
        CHECK(a.rect.right() <= vp.bounds.w + 1e-9);
        CHECK(a.rect.bottom() <= vp.bounds.h + 1e-9);
    }
    CHECK_NEAR(area, vp.bounds.w * vp.bounds.h, 1e-6);

    for (std::size_t i = 0; i < lf.size(); ++i)
        for (std::size_t j = i + 1; j < lf.size(); ++j)
            CHECK_MSG(!overlaps(lf[i].rect, lf[j].rect), "leaves {} and {} overlap", lf[i].node,
                      lf[j].node);
}

LTEST_CASE(leaves_do_not_overlap_once_dividers_have_thickness) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const std::vector<LeafRect> lf = l.leaves(vp, cs);
    double area = 0.0;
    for (const LeafRect& a : lf)
        area += a.rect.w * a.rect.h;
    // Dividers consume real space, so the leaves no longer fill the window.
    CHECK(area < vp.bounds.w * vp.bounds.h);

    for (std::size_t i = 0; i < lf.size(); ++i)
        for (std::size_t j = i + 1; j < lf.size(); ++j)
            CHECK(!overlaps(lf[i].rect, lf[j].rect));
}

LTEST_CASE(the_layout_is_resolution_independent) {
    // ADR-0015 rule 4: only relative proportions are stored, so the same tree at twice the size
    // gives exactly twice the rects. This is the property whose absence destroys an OBS layout
    // when it is restored on a different monitor.
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();

    const std::vector<LeafRect> small = l.leaves(proportional_vp(1280.0, 720.0), cs);
    const std::vector<LeafRect> large = l.leaves(proportional_vp(2560.0, 1440.0), cs);
    REQUIRE(small.size() == large.size());

    for (std::size_t i = 0; i < small.size(); ++i) {
        CHECK_EQ(small[i].node, large[i].node);
        CHECK_NEAR(small[i].rect.x * 2.0, large[i].rect.x, 1e-6);
        CHECK_NEAR(small[i].rect.y * 2.0, large[i].rect.y, 1e-6);
        CHECK_NEAR(small[i].rect.w * 2.0, large[i].rect.w, 1e-6);
        CHECK_NEAR(small[i].rect.h * 2.0, large[i].rect.h, 1e-6);
    }
}

LTEST_CASE(minimum_sizes_are_honoured_when_the_window_is_tight) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(1100.0, 700.0);
    REQUIRE(l.fits(vp, cs));

    for (const LeafRect& lf : l.leaves(vp, cs)) {
        const Node* n = l.find(lf.node);
        REQUIRE(n != nullptr);
        for (const std::string& p : n->tabs) {
            const PanelMin m = cs.get(p);
            CHECK_MSG(lf.rect.w + 1e-6 >= m.w, "panel '{}' got width {:.2f}, minimum {:.2f}", p,
                      lf.rect.w, m.w);
            CHECK_MSG(lf.rect.h + 1e-6 >= m.h, "panel '{}' got height {:.2f}, minimum {:.2f}", p,
                      lf.rect.h, m.h);
        }
    }
}

LTEST_CASE(a_centre_drop_makes_a_tabbed_group) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const NodeId target = l.node_of_panel("info");
    const DockResult r = l.dock("scenes", target, Zone::Center, vp, cs);
    CHECK_MSG(r.applied(), "{}", r.reason);
    CHECK(!l.validate().has_value());

    CHECK_EQ(l.node_of_panel("scenes"), target);
    CHECK_EQ(l.node_of_panel("scenes"), l.node_of_panel("info"));
    // The dropped panel becomes the visible tab: the user just put it there on purpose.
    CHECK_EQ(l.tab_index_of("scenes"), std::size_t{4});
    const Node* n = l.find(target);
    REQUIRE(n != nullptr);
    CHECK_EQ(n->active, std::size_t{4});
    CHECK_EQ(sorted_panels(l).size(), std::size_t{12});
}

LTEST_CASE(an_outer_drop_splits_the_target_on_the_right_axis_and_side) {
    const Constraints cs = Constraints::studio_defaults();
    // Dividers off: this case is about which way the space is cut and who ends up on which side.
    // What a divider costs is tested separately, in leaves_do_not_overlap_once_dividers_have_...
    const Viewport vp = proportional_vp(2560.0, 1440.0);

    struct Expect {
        Zone zone;
        bool leading;
        Axis axis;
    };
    const Expect cases[] = {
        {Zone::Left, true, Axis::Horizontal},
        {Zone::Right, false, Axis::Horizontal},
        {Zone::Top, true, Axis::Vertical},
        {Zone::Bottom, false, Axis::Vertical},
    };

    for (const Expect& e : cases) {
        Layout l = Layout::default_studio();
        const NodeId target = l.node_of_panel("program");
        const Rect before = l.rect_of(target, vp, cs).value();

        const DockResult r = l.dock("browser", target, e.zone, vp, cs);
        CHECK_MSG(r.applied(), "{} -> {}", to_string(e.zone), r.reason);
        CHECK(!l.validate().has_value());
        CHECK(l.has_panel("browser"));

        const Rect got = l.rect_of(r.node, vp, cs).value();
        const Rect prog = l.rect_of(l.node_of_panel("program"), vp, cs).value();

        // The new panel takes half the target's space, on the side the zone named.
        if (e.axis == Axis::Horizontal) {
            CHECK_NEAR(got.h, before.h, 1e-6);
            CHECK(e.leading ? got.x < prog.x : got.x > prog.x);
        } else {
            CHECK_NEAR(got.w, before.w, 1e-6);
            CHECK(e.leading ? got.y < prog.y : got.y > prog.y);
        }
        // The target keeps the remainder: the pair covers exactly what the target had, and
        // splits it evenly (ADR-0016 rule 3).
        const double covered = (e.axis == Axis::Horizontal) ? got.w + prog.w : got.h + prog.h;
        const double had = (e.axis == Axis::Horizontal) ? before.w : before.h;
        CHECK_NEAR(covered, had, 1e-6);
        CHECK_NEAR((e.axis == Axis::Horizontal) ? got.w : got.h, had * 0.5, 1e-6);
    }
}

LTEST_CASE(docking_along_the_parent_axis_does_not_nest_a_new_split) {
    // Without flattening, every dock adds a level and the dividers stop lining up. The centre column
    // of the default layout is a vertical split, so a Top drop on one of its members should
    // widen that split rather than nest inside it.
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(3840.0, 2160.0);

    const NodeId program = l.node_of_panel("program");
    const NodeId row = l.find(program)->parent;
    REQUIRE(l.find(row) != nullptr);
    const std::size_t row_children = l.find(row)->children.size();
    const std::size_t nodes_before = l.node_count();
    const std::size_t depth_before = depth(l, l.root());

    const DockResult r = l.dock("browser", program, Zone::Top, vp, cs);
    REQUIRE(r.applied());
    CHECK(!l.validate().has_value());

    CHECK_EQ(l.node_count(), nodes_before + 1);       // one new leaf, no new split
    CHECK_EQ(depth(l, l.root()), depth_before);       // and no new level
    CHECK_EQ(l.find(l.node_of_panel("browser"))->parent, row);
    CHECK_EQ(l.find(row)->children.size(), row_children + 1);

    // Across the axis, a new split is unavoidable and is the correct answer.
    const std::size_t nodes_mid = l.node_count();
    const DockResult r2 = l.dock("chat", l.node_of_panel("program"), Zone::Right, vp, cs);
    REQUIRE(r2.applied());
    CHECK_EQ(l.node_count(), nodes_mid + 2); // a leaf and the split that holds the pair
    CHECK(!l.validate().has_value());
}

LTEST_CASE(repeated_docking_keeps_the_tree_shallow) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(7680.0, 4320.0);

    for (int i = 0; i < 12; ++i) {
        const std::string name = std::format("extra-{}", i);
        const DockResult r = l.dock(name, l.node_of_panel("program"), Zone::Right, vp, cs);
        CHECK_MSG(r.applied(), "{}: {}", name, r.reason);
        CHECK(!l.validate().has_value());
    }
    // Twelve docks along one axis: a flattened tree gains breadth, not depth.
    CHECK_MSG(depth(l, l.root()) <= 4, "depth grew to {}", depth(l, l.root()));
}

LTEST_CASE(emptying_a_group_collapses_it_and_the_sibling_takes_the_space) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    REQUIRE(l.close_panel("favourites"));
    const NodeId column = l.find(l.node_of_panel("scenes"))->parent; // the scenes/sources column
    const Rect column_rect = l.rect_of(column, vp, cs).value();
    const std::size_t nodes_before = l.node_count();

    const DockResult r = l.dock("scenes", l.node_of_panel("info"), Zone::Center, vp, cs);
    REQUIRE(r.applied());
    CHECK(!l.validate().has_value());

    // The scenes leaf is gone and so is the column that held only sources.
    CHECK_EQ(l.node_count(), nodes_before - 2);
    CHECK(l.find(column) == nullptr);

    const Rect sources = l.rect_of(l.node_of_panel("sources"), vp, cs).value();
    CHECK_NEAR(sources.x, column_rect.x, 1e-6);
    CHECK_NEAR(sources.y, column_rect.y, 1e-6);
    CHECK_NEAR(sources.w, column_rect.w, 1e-6);
    CHECK_NEAR(sources.h, column_rect.h, 1e-6);
}

LTEST_CASE(dropping_a_panel_where_it_already_is_does_nothing) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    REQUIRE(l.activate("properties"));
    const Layout before = l;

    // Its own group, centre zone, *and already the visible tab*: nothing at all to do. Dropping
    // a tab that is not the active one would bring it forward, which is a real change.
    const DockResult a = l.dock("properties", l.node_of_panel("properties"), Zone::Center, vp,
                                cs);
    CHECK_EQ(a.status, DockStatus::NoOp);
    CHECK(l == before);

    // Its own single-tab group, outer zone: a panel cannot be split away from itself.
    const DockResult b = l.dock("program", l.node_of_panel("program"), Zone::Left, vp, cs);
    CHECK_EQ(b.status, DockStatus::NoOp);
    CHECK(l == before);
}

LTEST_CASE(pulling_one_tab_out_of_a_group_splits_the_group) {
    // Rule 5 of ADR-0016: dragging a tab out of a group detaches it. The inverse of a centre
    // drop has to work or the model is not learnable.
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const NodeId group = l.node_of_panel("info");
    REQUIRE(l.node_of_panel("log") == group);

    const DockResult r = l.dock("log", group, Zone::Bottom, vp, cs);
    CHECK_MSG(r.applied(), "{}", r.reason);
    CHECK(!l.validate().has_value());
    CHECK(l.node_of_panel("log") != l.node_of_panel("info"));

    const Rect stats = l.rect_of(l.node_of_panel("info"), vp, cs).value();
    const Rect log = l.rect_of(l.node_of_panel("log"), vp, cs).value();
    CHECK(log.y > stats.y);
    CHECK_NEAR(log.w, stats.w, 1e-6);
}

LTEST_CASE(a_drop_that_would_starve_a_panel_is_refused_and_changes_nothing) {
    // ADR-0016 rule 8: layouts fail at the gesture, visibly, rather than producing a three-pixel
    // panel the user then has to find and fix.
    Constraints cs;
    cs.set_fallback(PanelMin{180.0, 110.0});
    const Viewport vp = vp_of(700.0, 400.0);

    Layout l;
    REQUIRE(l.dock("a", l.root(), Zone::Center, vp, cs).applied());
    REQUIRE(l.dock("b", l.node_of_panel("a"), Zone::Right, vp, cs).applied());
    REQUIRE(l.dock("c", l.node_of_panel("b"), Zone::Right, vp, cs).applied());
    CHECK(l.fits(vp, cs));

    const Layout before = l;
    const DockResult r = l.dock("d", l.node_of_panel("c"), Zone::Right, vp, cs);
    CHECK_EQ(r.status, DockStatus::Rejected);
    CHECK(!r.reason.empty());
    CHECK(!l.has_panel("d"));
    CHECK_MSG(l == before, "a refused dock mutated the layout");

    // The same gesture in a window with room for it succeeds.
    const DockResult ok = l.dock("d", l.node_of_panel("c"), Zone::Right, vp_of(1200.0, 400.0), cs);
    CHECK_MSG(ok.applied(), "{}", ok.reason);
}

LTEST_CASE(a_panel_not_yet_in_the_tree_can_be_docked) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    CHECK(!l.has_panel("chat"));
    const DockResult r = l.dock("chat", l.node_of_panel("audio-mixer"), Zone::Right, vp, cs);
    REQUIRE(r.applied());
    CHECK(l.has_panel("chat"));
    CHECK_EQ(sorted_panels(l).size(), std::size_t{13});
    CHECK(!l.validate().has_value());
}

LTEST_CASE(an_edge_dock_makes_a_full_height_column) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const DockResult r = l.dock_edge("chat", Edge::Right, vp, cs);
    CHECK_MSG(r.applied(), "{}", r.reason);
    CHECK(!l.validate().has_value());

    const Rect chat = l.rect_of(r.node, vp, cs).value();
    CHECK_NEAR(chat.h, vp.bounds.h, 1e-6);
    CHECK_NEAR(chat.right(), vp.bounds.right(), 1e-6);
    // Flattening into the horizontal root shares the width left after all its dividers.
    const auto dividers = static_cast<double>(l.find(l.root())->children.size() - 1);
    CHECK_NEAR(chat.w, (vp.bounds.w - dividers * vp.divider_px) * vp.edge_share, 1e-6);

    // And nothing else reaches into its column.
    for (const LeafRect& lf : l.leaves(vp, cs)) {
        if (lf.node == r.node)
            continue;
        CHECK(lf.rect.right() <= chat.x + 1e-6);
    }
}

LTEST_CASE(an_edge_dock_on_the_same_axis_as_the_root_flattens) {
    Layout l;
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    REQUIRE(l.dock("a", l.root(), Zone::Center, vp, cs).applied());

    REQUIRE(l.dock_edge("b", Edge::Left, vp, cs).applied());
    const std::size_t nodes = l.node_count();
    REQUIRE(l.dock_edge("c", Edge::Left, vp, cs).applied());

    CHECK_EQ(l.node_count(), nodes + 1); // a leaf, not a leaf plus a nested split
    CHECK_EQ(depth(l, l.root()), std::size_t{2});
    CHECK(!l.validate().has_value());

    const Rect c = l.rect_of(l.node_of_panel("c"), vp, cs).value();
    const Rect b = l.rect_of(l.node_of_panel("b"), vp, cs).value();
    CHECK(c.x < b.x);
}

LTEST_CASE(closing_panels_collapses_the_tree_down_to_an_empty_root) {
    Layout l = Layout::default_studio();
    CHECK(!l.close_panel("not-a-panel"));

    for (const std::string& p : l.panels()) {
        CHECK(l.close_panel(p));
        const auto bad = l.validate();
        CHECK_MSG(!bad.has_value(), "after closing '{}': {}", p, bad.value_or(""));
    }
    CHECK(l.panels().empty());
    CHECK_EQ(l.node_count(), std::size_t{1});
    CHECK(!l.validate().has_value());

    // And the workspace is still usable: a panel can be docked back into it.
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    CHECK(l.dock("program", l.root(), Zone::Center, vp, cs).applied());
}

LTEST_CASE(dividers_are_reported_and_draggable) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    std::size_t expected = 0;
    for (const NodeId id : l.node_ids()) {
        const Node* n = l.find(id);
        if (n && n->is_split())
            expected += n->children.size() - 1;
    }
    const std::vector<DividerHandle> ds = l.dividers(vp, cs);
    CHECK_EQ(ds.size(), expected);
    for (const DividerHandle& d : ds) {
        CHECK_NEAR(d.axis == Axis::Horizontal ? d.rect.w : d.rect.h, vp.divider_px, 1e-6);
        const auto hit = l.hit_test_divider(d.rect.x + d.rect.w * 0.5,
                                            d.rect.y + d.rect.h * 0.5, vp, cs);
        REQUIRE(hit.has_value());
        CHECK_EQ(hit->split, d.split);
        CHECK_EQ(hit->index, d.index);
    }
}

LTEST_CASE(dragging_a_divider_moves_it_and_clamps_at_the_minimum) {
    Constraints cs;
    cs.set_fallback(PanelMin{180.0, 110.0});
    const Viewport vp = vp_of(1200.0, 400.0);

    Layout l;
    REQUIRE(l.dock("a", l.root(), Zone::Center, vp, cs).applied());
    REQUIRE(l.dock("b", l.node_of_panel("a"), Zone::Right, vp, cs).applied());

    const NodeId split = l.find(l.node_of_panel("a"))->parent;
    const double w0 = l.rect_of(l.node_of_panel("a"), vp, cs).value().w;

    CHECK(l.move_divider(split, 0, 100.0, vp, cs));
    CHECK_NEAR(l.rect_of(l.node_of_panel("a"), vp, cs).value().w, w0 + 100.0, 1e-6);
    CHECK(!l.validate().has_value());

    // Shove it far past the minimum: it clamps rather than inverting or starving the neighbour.
    CHECK(l.move_divider(split, 0, -10000.0, vp, cs));
    CHECK_NEAR(l.rect_of(l.node_of_panel("a"), vp, cs).value().w, 180.0, 1e-6);
    // Already at the stop, so there is nothing left to move.
    CHECK(!l.move_divider(split, 0, -10000.0, vp, cs));
    CHECK(!l.validate().has_value());

    CHECK(!l.move_divider(split, 5, 10.0, vp, cs));          // no such divider
    CHECK(!l.move_divider(l.node_of_panel("a"), 0, 10.0, vp, cs)); // not a split
}

LTEST_CASE(hit_testing_prefers_the_workspace_edges) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const auto left = l.hit_test(4.0, 700.0, vp, cs);
    REQUIRE(left.has_value());
    CHECK(left->is_edge);
    CHECK_EQ(left->edge, Edge::Left);
    CHECK_NEAR(left->preview.h, vp.bounds.h, 1e-6);

    const auto bottom = l.hit_test(1200.0, vp.bounds.bottom() - 2.0, vp, cs);
    REQUIRE(bottom.has_value());
    CHECK(bottom->is_edge);
    CHECK_EQ(bottom->edge, Edge::Bottom);

    CHECK(!l.hit_test(-5.0, 100.0, vp, cs).has_value());
    CHECK(!l.hit_test(100.0, 99999.0, vp, cs).has_value());
}

LTEST_CASE(hit_testing_inside_a_panel_names_the_panel_and_the_zone) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const NodeId program = l.node_of_panel("program");
    const Rect r = l.rect_of(program, vp, cs).value();

    const auto centre = l.hit_test(r.x + r.w * 0.5, r.y + r.h * 0.5, vp, cs);
    REQUIRE(centre.has_value());
    CHECK(!centre->is_edge);
    CHECK_EQ(centre->node, program);
    CHECK_EQ(centre->zone, Zone::Center);
    CHECK_NEAR(centre->preview.w, r.w, 1e-6);

    const auto left = l.hit_test(r.x + 6.0, r.y + r.h * 0.5, vp, cs);
    REQUIRE(left.has_value());
    CHECK_EQ(left->node, program);
    CHECK_EQ(left->zone, Zone::Left);
    CHECK_NEAR(left->preview.w, r.w * 0.5, 1e-6);
}

LTEST_CASE(a_hit_test_result_can_be_docked_directly) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const Rect r = l.rect_of(l.node_of_panel("audio-mixer"), vp, cs).value();
    const auto t = l.hit_test(r.x + r.w * 0.5, r.y + 4.0, vp, cs);
    REQUIRE(t.has_value());
    CHECK_EQ(t->zone, Zone::Top);

    const DockResult d = l.dock("chat", *t, vp, cs);
    CHECK_MSG(d.applied(), "{}", d.reason);
    CHECK(!l.validate().has_value());
    const Rect chat = l.rect_of(d.node, vp, cs).value();
    CHECK_NEAR(chat.y, r.y, 1e-6);
    CHECK(chat.h < r.h);

    CHECK_NEAR(chat.h, (r.h - vp.divider_px) * 0.5, 1e-6);

    // What the highlight promised is what the drop produced. The highlight is drawn before the
    // divider exists, so it overstates by half a divider -- three pixels on a panel this size.
    // That is left alone deliberately: teaching zone_preview about divider thickness would
    // couple the drag overlay to the layout engine's spacing to hide a sub-pixel-per-edge
    // difference nobody can see.
    CHECK_NEAR(chat.x, t->preview.x, 1e-6);
    CHECK_NEAR(chat.w, t->preview.w, 1e-6);
    CHECK_NEAR(chat.h, t->preview.h, vp.divider_px);
}

LTEST_CASE(activating_a_tab_selects_it) {
    Layout l = Layout::default_studio();
    CHECK(l.activate("log"));
    CHECK_EQ(l.find(l.node_of_panel("log"))->active, std::size_t{2});
    CHECK(l.activate("info"));
    CHECK_EQ(l.find(l.node_of_panel("info"))->active, std::size_t{1});
    CHECK(!l.activate("nope"));
}

LTEST_CASE(closing_the_visible_tab_leaves_a_valid_selection) {
    Layout l = Layout::default_studio();
    const NodeId group = l.node_of_panel("info");
    REQUIRE(l.activate("log"));
    CHECK_EQ(l.find(group)->active, std::size_t{2});

    // Closing the visible tab leaves the selection on a tab that still exists.
    CHECK(l.close_panel("log"));
    const Node* n = l.find(l.node_of_panel("info"));
    REQUIRE(n != nullptr);
    CHECK_EQ(n->tabs.size(), std::size_t{3});
    CHECK_EQ(n->active, std::size_t{2});
    CHECK(!l.validate().has_value());
}

LTEST_CASE(bad_arguments_are_refused_rather_than_asserted) {
    Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    const Layout before = l;

    CHECK_EQ(l.dock("", l.node_of_panel("program"), Zone::Center, vp, cs).status,
             DockStatus::Rejected);
    CHECK_EQ(l.dock("x", l.node_of_panel("program"), Zone::None, vp, cs).status,
             DockStatus::Rejected);
    CHECK_EQ(l.dock("x", 99999, Zone::Center, vp, cs).status, DockStatus::Rejected);
    // A split is not a drop target; only panel groups are.
    CHECK_EQ(l.dock("x", l.find(l.node_of_panel("program"))->parent, Zone::Center, vp, cs).status,
             DockStatus::Rejected);
    CHECK(l == before);
}

LTEST_CASE(the_neighbour_in_a_direction_is_whatever_is_drawn_there) {
    // Geometric, not structural. The panel to the left is whatever is drawn to the left, not
    // whatever happens to be a sibling: a tree-walking answer would skip past something visibly
    // adjacent because it lives in a different branch.
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    const NodeId program = l.node_of_panel("program");
    const NodeId scenes = l.node_of_panel("scenes");
    const NodeId sources = l.node_of_panel("sources");
    const NodeId inspect = l.node_of_panel("info");
    const NodeId mixer = l.node_of_panel("audio-mixer");

    // scenes sits above sources in the left column; program is to their right.
    CHECK(l.neighbour(scenes, Edge::Bottom, vp, cs).value() == sources);
    CHECK(l.neighbour(sources, Edge::Top, vp, cs).value() == scenes);
    CHECK(l.neighbour(program, Edge::Right, vp, cs).value() == inspect);
    CHECK(l.neighbour(inspect, Edge::Left, vp, cs).value() == program);

    // Across a branch: the panel left of program is in the scenes/sources column, which is a
    // different subtree entirely.
    const auto left_of_program = l.neighbour(program, Edge::Left, vp, cs);
    REQUIRE(left_of_program.has_value());
    CHECK_MSG(*left_of_program == scenes || *left_of_program == sources,
              "the neighbour across a branch boundary was not found");

    // And the bottom row is below the top one.
    CHECK(l.neighbour(program, Edge::Bottom, vp, cs).value() == mixer);
}

LTEST_CASE(there_is_no_neighbour_past_the_edge_of_the_workspace) {
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    CHECK(!l.neighbour(l.node_of_panel("scenes"), Edge::Left, vp, cs).has_value());
    CHECK(!l.neighbour(l.node_of_panel("scenes"), Edge::Top, vp, cs).has_value());
    CHECK(!l.neighbour(l.node_of_panel("info"), Edge::Right, vp, cs).has_value());
    CHECK(!l.neighbour(l.node_of_panel("audio-mixer"), Edge::Bottom, vp, cs).has_value());
    CHECK(!l.neighbour(99999, Edge::Left, vp, cs).has_value());
}

LTEST_CASE(a_panel_is_never_its_own_neighbour_and_corners_do_not_count) {
    // A panel diagonally across the workspace is not "the one to the left", however far left it
    // is. Without the overlap test, moving a panel by keyboard would teleport it across the
    // screen.
    Layout l;
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(1200.0, 800.0);

    REQUIRE(l.dock("a", l.root(), Zone::Center, vp, cs).applied());
    REQUIRE(l.dock("b", l.node_of_panel("a"), Zone::Right, vp, cs).applied());
    REQUIRE(l.dock("c", l.node_of_panel("a"), Zone::Bottom, vp, cs).applied());
    // Layout: a top-left, b right (full height), c below a.

    for (const Edge e : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom}) {
        const auto n = l.neighbour(l.node_of_panel("a"), e, vp, cs);
        if (n)
            CHECK_MSG(*n != l.node_of_panel("a"), "a panel was its own neighbour");
    }
    // c is below a, and b is to the right of both.
    CHECK(l.neighbour(l.node_of_panel("a"), Edge::Bottom, vp, cs).value() ==
          l.node_of_panel("c"));
    CHECK(l.neighbour(l.node_of_panel("a"), Edge::Right, vp, cs).value() ==
          l.node_of_panel("b"));
}

LTEST_CASE(walking_in_one_direction_reaches_every_panel_and_terminates) {
    // The property that makes keyboard movement usable: repeatedly stepping one way gets you
    // somewhere and then stops, rather than cycling between two panels forever.
    const Layout l = Layout::default_studio();
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    for (const Edge e : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom}) {
        for (const LeafRect& start : l.leaves(vp, cs)) {
            std::set<NodeId> seen{start.node};
            NodeId at = start.node;
            int steps = 0;
            while (const auto next = l.neighbour(at, e, vp, cs)) {
                REQUIRE_MSG(seen.insert(*next).second,
                            "walking {} revisited a panel, so the walk does not terminate",
                            to_string(e));
                at = *next;
                REQUIRE_MSG(++steps < 32, "walking {} did not terminate", to_string(e));
            }
        }
    }
}

LTEST_CASE(a_long_random_sequence_of_gestures_never_breaks_an_invariant) {
    // The gesture is forgiving by design, which means the engine will be driven into states no
    // hand-written case would think of. A fixed seed keeps any failure reproducible.
    //
    // Two window sizes, because they exercise different code: the large one applies nearly every
    // dock and hammers the tree surgery, while the small one spends most of its time refusing
    // drops and so tests that a refusal really does leave the layout alone.
    // Must contain every panel default_studio() starts with, plus the ones a gesture may add:
    // the walk asserts that nothing appears in the tree that was not asked for, so a panel in
    // the starting layout but missing here fails on the first step.
    const std::vector<std::string> pool = {"program",  "scenes",   "sources",     "info",
                                           "log",      "controls", "audio-mixer", "properties",
                                           "outputs",  "chat",     "browser",
                                           "replay",   "broadcast-stats", "favourites", "countdowns"};
    const Constraints cs = Constraints::studio_defaults();

    for (const Viewport& vp : {vp_of(3840.0, 2160.0), vp_of(1366.0, 768.0)}) {
        std::mt19937 rng{20261012u};
        Layout l = Layout::default_studio();
        REQUIRE_MSG(l.fits(vp, cs), "the default layout does not fit {:.0f}x{:.0f}", vp.bounds.w,
                    vp.bounds.h);

        int applied = 0;
        int refused = 0;
        int noop = 0;
        int closed = 0;

        for (int step = 0; step < 4000; ++step) {
            const Layout before = l;
            const std::string& panel = pool[rng() % pool.size()];
            const auto roll = rng() % 100;

            if (roll < 12) {
                closed += l.close_panel(panel) ? 1 : 0;
            } else if (roll < 22) {
                const Edge e = static_cast<Edge>(rng() % 4);
                const DockResult r = l.dock_edge(panel, e, vp, cs);
                if (r.status == DockStatus::Rejected) {
                    ++refused;
                    CHECK_MSG(l == before, "refused edge dock mutated the layout at step {}",
                              step);
                } else if (r.status == DockStatus::NoOp) {
                    ++noop;
                } else {
                    ++applied;
                }
            } else {
                const std::vector<LeafRect> lf = l.leaves(vp, cs);
                if (lf.empty()) {
                    CHECK(l.dock(panel, l.root(), Zone::Center, vp, cs).ok());
                    continue;
                }
                const NodeId target = lf[rng() % lf.size()].node;
                const Zone zone = static_cast<Zone>(1 + rng() % 5);
                const DockResult r = l.dock(panel, target, zone, vp, cs);
                if (r.status == DockStatus::Rejected) {
                    ++refused;
                    CHECK_MSG(l == before, "refused dock mutated the layout at step {}", step);
                    CHECK(!r.reason.empty());
                } else if (r.status == DockStatus::NoOp) {
                    ++noop;
                    CHECK_MSG(l == before, "a no-op mutated the layout at step {}", step);
                } else {
                    ++applied;
                    CHECK(l.has_panel(panel));
                }
            }

            const auto bad = l.validate();
            REQUIRE_MSG(!bad.has_value(), "step {}: {}", step, bad.value_or(""));

            // No panel is ever duplicated, and nothing appears that was not asked for.
            const std::vector<std::string> ps = l.panels();
            const std::set<std::string> uniq(ps.begin(), ps.end());
            CHECK_EQ(uniq.size(), ps.size());
            for (const std::string& p : ps)
                CHECK(std::find(pool.begin(), pool.end(), p) != pool.end());

            // Flattening keeps the tree from growing without bound. The bound is structural
            // rather than a tuned number: a tree cannot have more levels than it has leaves, so
            // exceeding it means nesting that nothing in the layout justifies. The tight check
            // on flattening itself lives in repeated_docking_keeps_the_tree_shallow, where a
            // run of same-axis docks must stay four deep however many there are.
            CHECK_MSG(depth(l, l.root()) <= pool.size() + 1, "depth {} at step {}",
                      depth(l, l.root()), step);
            CHECK(l.node_count() <= 2 * pool.size() + 2);

            // Every panel that is in the tree can actually be drawn: no zero-area leaves, and
            // nothing outside the window.
            for (const LeafRect& leaf : l.leaves(vp, cs)) {
                CHECK(leaf.rect.w > 0.0);
                CHECK(leaf.rect.h > 0.0);
                CHECK(leaf.rect.x >= -1e-6);
                CHECK(leaf.rect.y >= -1e-6);
                CHECK(leaf.rect.right() <= vp.bounds.w + 1e-6);
                CHECK(leaf.rect.bottom() <= vp.bounds.h + 1e-6);
            }
        }

        std::fprintf(stderr, "       %.0fx%.0f: applied %d, no-op %d, refused %d, closed %d\n",
                     vp.bounds.w, vp.bounds.h, applied, noop, refused, closed);
        CHECK_MSG(applied > 500, "only {} docks applied", applied);
        CHECK_MSG(closed > 100, "only {} panels closed", closed);
    }
}

LTEST_CASE(a_tight_window_refuses_drops_without_corrupting_anything) {
    // The same loop as above, but small enough that refusals are the common case. The point is
    // the refusal path, so assert it was actually taken.
    const std::vector<std::string> pool = {"program", "scenes",  "sources", "info",
                                           "log",     "chat",    "browser", "replay",
                                           "properties", "audio-mixer", "controls"};
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(1366.0, 768.0);

    std::mt19937 rng{7u};
    Layout l = Layout::default_studio();
    int refused = 0;

    for (int step = 0; step < 2000; ++step) {
        const Layout before = l;
        const std::string& panel = pool[rng() % pool.size()];
        const std::vector<LeafRect> lf = l.leaves(vp, cs);
        if (lf.empty()) {
            REQUIRE(l.dock(panel, l.root(), Zone::Center, vp, cs).ok());
            continue;
        }
        const NodeId target = lf[rng() % lf.size()].node;
        const Zone zone = static_cast<Zone>(1 + rng() % 5);
        const DockResult r = l.dock(panel, target, zone, vp, cs);
        if (r.status == DockStatus::Rejected) {
            ++refused;
            CHECK_MSG(l == before, "refused dock mutated the layout at step {}", step);
        }
        const auto bad = l.validate();
        REQUIRE_MSG(!bad.has_value(), "step {}: {}", step, bad.value_or(""));
        CHECK(l.fits(vp, cs));
    }
    std::fprintf(stderr, "       refused %d of 2000\n", refused);
    CHECK_MSG(refused > 100, "only {} drops were refused; the refusal path is barely tested",
              refused);
}

LTEST_MAIN("layout")
