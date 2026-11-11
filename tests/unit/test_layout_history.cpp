// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout undo (ADR-0015 rule 6).

#include "llayout/history.hpp"

#include "ltest.hpp"

#include <random>

using namespace llayout;

namespace {

Viewport vp_of(double w, double h) {
    Viewport vp;
    vp.bounds = Rect{0.0, 0.0, w, h};
    return vp;
}

/// The call shape the UI uses: mutate a copy, commit only if it applied.
bool try_dock(History& h, std::string_view panel, Zone zone, const Viewport& vp,
              const Constraints& cs) {
    Layout next = h.current();
    const NodeId target = next.node_of_panel("program");
    if (!next.dock(panel, target, zone, vp, cs).applied())
        return false;
    return h.commit(std::move(next));
}

} // namespace

LTEST_CASE(a_fresh_history_has_nothing_to_undo) {
    const History h{Layout::default_studio()};
    CHECK(!h.can_undo());
    CHECK(!h.can_redo());
    CHECK_EQ(h.undo_depth(), std::size_t{0});
    CHECK(h.current() == Layout::default_studio());
}

LTEST_CASE(a_bad_drag_costs_one_keystroke) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    History h{Layout::default_studio()};
    const Layout original = h.current();

    CHECK(try_dock(h, "chat", Zone::Bottom, vp, cs));
    CHECK(h.can_undo());
    CHECK(h.current().has_panel("chat"));
    CHECK(!(h.current() == original));

    CHECK(h.undo());
    CHECK_MSG(h.current() == original, "undo did not restore the layout exactly");
    CHECK(!h.can_undo());
    CHECK(h.can_redo());

    CHECK(h.redo());
    CHECK(h.current().has_panel("chat"));
    CHECK(!h.redo());
}

LTEST_CASE(a_no_op_does_not_spend_an_undo_step) {
    // Otherwise Ctrl+Z after a mis-aimed drag that did nothing would appear to do nothing either,
    // and the operator presses it again, and the second press undoes real work.
    History h{Layout::default_studio()};
    CHECK(!h.commit(Layout::default_studio()));
    CHECK(!h.can_undo());
    CHECK_EQ(h.undo_depth(), std::size_t{0});
}

LTEST_CASE(a_new_step_discards_the_redo_tail) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    History h{Layout::default_studio()};

    REQUIRE(try_dock(h, "chat", Zone::Bottom, vp, cs));
    REQUIRE(h.undo());
    CHECK(h.can_redo());

    REQUIRE(try_dock(h, "browser", Zone::Right, vp, cs));
    CHECK(!h.can_redo());
    CHECK(h.current().has_panel("browser"));
    CHECK(!h.current().has_panel("chat"));
}

LTEST_CASE(the_stack_is_bounded_and_the_oldest_steps_fall_off) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(7680.0, 4320.0);
    History h{Layout::default_studio(), 8};

    // Enough to overflow a stack of 8 several times, but not so many that the top row runs
    // out of width and the dock starts being refused for a different reason.
    for (int i = 0; i < 24; ++i)
        REQUIRE(try_dock(h, std::format("p-{}", i), Zone::Right, vp, cs));

    CHECK_EQ(h.undo_depth(), std::size_t{8});
    for (int i = 0; i < 8; ++i)
        CHECK(h.undo());
    CHECK(!h.can_undo());
    CHECK_EQ(h.redo_depth(), std::size_t{8});

    // Whatever is left on the stack is still a valid layout; a bounded stack must not hand back
    // a truncated tree.
    CHECK(!h.current().validate().has_value());
}

LTEST_CASE(reset_forgets_everything) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    History h{Layout::default_studio()};
    REQUIRE(try_dock(h, "chat", Zone::Bottom, vp, cs));
    REQUIRE(h.undo());

    h.reset(Layout::default_studio());
    CHECK(!h.can_undo());
    CHECK(!h.can_redo());
    CHECK(h.current() == Layout::default_studio());
}

LTEST_CASE(undo_and_redo_survive_a_long_random_walk) {
    const std::vector<std::string> pool = {"program", "scenes", "sources", "stats",
                                           "log",     "chat",   "browser", "replay"};
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(3840.0, 2160.0);

    std::mt19937 rng{99u};
    History h{Layout::default_studio(), 16};
    int undone = 0;
    int redone = 0;

    for (int step = 0; step < 3000; ++step) {
        const auto roll = rng() % 100;
        if (roll < 20) {
            undone += h.undo() ? 1 : 0;
        } else if (roll < 35) {
            redone += h.redo() ? 1 : 0;
        } else {
            Layout next = h.current();
            const auto leaves = next.leaves(vp, cs);
            if (leaves.empty()) {
                REQUIRE(next.dock(pool[rng() % pool.size()], next.root(), Zone::Center, vp, cs)
                            .ok());
                h.commit(std::move(next));
                continue;
            }
            const NodeId target = leaves[rng() % leaves.size()].node;
            const Zone zone = static_cast<Zone>(1 + rng() % 5);
            if (next.dock(pool[rng() % pool.size()], target, zone, vp, cs).applied())
                h.commit(std::move(next));
        }
        const auto bad = h.current().validate();
        REQUIRE_MSG(!bad.has_value(), "step {}: {}", step, bad.value_or(""));
        CHECK(h.undo_depth() <= 16);
        CHECK(h.redo_depth() <= 16);
    }
    CHECK(undone > 100);
    CHECK(redone > 50);
    std::fprintf(stderr, "       (undone %d, redone %d)\n", undone, redone);
}

LTEST_MAIN("layout-history")
