// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ltest.hpp"

#include "llayout/serialize.hpp"
#include "llayout/workspaces.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using namespace llayout;

namespace {

std::vector<std::string> names_of(const Workspaces& w) {
    std::vector<std::string> out;
    for (const Workspace& item : w.items())
        out.push_back(item.name);
    return out;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

std::vector<std::string> sorted_panels(const Layout& l) {
    std::vector<std::string> p = l.panels();
    std::sort(p.begin(), p.end());
    return p;
}

} // namespace

LTEST_CASE(the_defaults_are_the_six_built_ins_and_all_are_valid) {
    const Workspaces w = Workspaces::defaults();
    CHECK_EQ(names_of(w), (std::vector<std::string>{"Build", "Live", "Encode", "Media", "Edit",
                                                    "Graphics"}));
    CHECK_EQ(w.active(), std::size_t{0});
    CHECK_MSG(!w.validate().has_value(), "{}", w.validate().value_or(""));
    for (const Workspace& item : w.items())
        CHECK_MSG(item.builtin, "'{}' should be built in", item.name);
}

LTEST_CASE(build_and_live_hold_different_panels_for_different_jobs) {
    // The whole point of there being two. If these ever become the same set, the feature has
    // quietly stopped doing anything.
    const Layout build = Layout::default_build();
    const Layout live = Layout::default_live();
    CHECK(sorted_panels(build) != sorted_panels(live));

    const std::vector<std::string> b = sorted_panels(build);
    const std::vector<std::string> v = sorted_panels(live);
    const auto has = [](const std::vector<std::string>& in, const char* p) {
        return std::find(in.begin(), in.end(), p) != in.end();
    };

    // Building: the saved operator arrangement includes monitoring alongside the editing lists.
    CHECK(has(b, "scenes"));
    CHECK(has(b, "sources"));
    CHECK(has(b, "properties"));
    CHECK(has(b, "broadcast-stats"));
    CHECK(has(b, "outputs"));
    CHECK(has(b, "countdowns"));

    // Live: favourites instead of the full lists, and the things that say how it is going.
    CHECK(has(v, "favourites"));
    CHECK(has(v, "broadcast-stats"));
    CHECK(has(v, "outputs"));
    CHECK_MSG(!has(v, "sources"), "the live workspace still shows the source list");
    CHECK_MSG(!has(v, "properties"), "the live workspace still shows the properties panel");

    // Both have to show the show.
    CHECK(has(b, "program"));
    CHECK(has(v, "program"));
    CHECK(!build.validate().has_value());
    CHECK(!live.validate().has_value());
}

LTEST_CASE(a_built_in_workspace_cannot_be_renamed_or_removed_but_can_be_reset) {
    Workspaces w = Workspaces::defaults();

    CHECK_MSG(!w.rename(0, "Setup").has_value(), "a built-in workspace was renamed");
    CHECK_MSG(!w.remove(0).has_value(), "a built-in workspace was removed");
    CHECK_EQ(names_of(w), (std::vector<std::string>{"Build", "Live", "Encode", "Media", "Edit",
                                                    "Graphics"}));

    // It can be rearranged, which is the part that must keep working.
    Layout rearranged = w.items()[1].layout;
    REQUIRE(rearranged.close_panel("outputs"));
    REQUIRE(w.set_layout(1, rearranged).has_value());
    CHECK(!w.items()[1].layout.has_panel("outputs"));

    // And put back.
    REQUIRE(w.reset(1).has_value());
    CHECK_MSG(w.items()[1].layout.has_panel("outputs"), "resetting Live did not restore it");
    CHECK_MSG(w.items()[1].name == "Live", "resetting renamed it");
}

LTEST_CASE(a_workspace_can_be_added_renamed_and_removed) {
    Workspaces w = Workspaces::defaults();
    // Appended after the built-ins, however many there are: a test that hardcodes the index
    // has to be edited every time one is added, which is how it comes to be edited carelessly.
    const std::size_t builtins = w.size();
    const auto added = w.add("Editing", Layout::default_build());
    REQUIRE(added.has_value());
    CHECK_EQ(*added, builtins);
    CHECK(!w.items()[builtins].builtin);

    REQUIRE(w.rename(builtins, "Editing Bay").has_value());
    CHECK_EQ(w.items()[builtins].name, std::string{"Editing Bay"});

    REQUIRE(w.remove(builtins).has_value());
    CHECK_EQ(w.size(), builtins);
}

LTEST_CASE(two_workspaces_cannot_share_a_name) {
    // Two tabs reading "Live" is the kind of thing that only hurts while something else has
    // your attention.
    Workspaces w = Workspaces::defaults();
    const std::size_t builtins = w.size();
    CHECK(!w.add("Live", Layout::default_live()).has_value());
    CHECK(!w.add("", Layout::default_build()).has_value());
    REQUIRE(w.add("Second", Layout::default_build()).has_value());
    REQUIRE(w.add("Third", Layout::default_build()).has_value());
    CHECK(!w.add("Encode", Layout::default_build()).has_value());
    // Every built-in name is taken, not only the first three.
    CHECK(!w.add("Media", Layout::default_media()).has_value());
    CHECK(!w.add("Edit", Layout::default_edit()).has_value());
    CHECK(!w.add("Graphics", Layout::default_graphics()).has_value());
    CHECK_MSG(!w.rename(builtins + 1, "Second").has_value(),
              "two workspaces were given the same name");
    CHECK_EQ(w.size(), builtins + 2);
}

LTEST_CASE(removing_the_active_workspace_leaves_a_real_one_active) {
    Workspaces w = Workspaces::defaults();
    const std::size_t builtins = w.size();
    REQUIRE(w.add("Third", Layout::default_build()).has_value());
    REQUIRE(w.add("Fourth", Layout::default_build()).has_value());

    // The last one, whatever index that is: "Fourth" is what is being removed, not index 4.
    REQUIRE(w.set_active(builtins + 1));
    REQUIRE(w.remove(builtins + 1).has_value());
    CHECK_MSG(w.active() < w.size(), "the active index points past the end");
    CHECK_EQ(w.items()[w.active()].name, std::string{"Third"});

    // Removing one *before* the active one has to shift the index, not leave it pointing at a
    // neighbour.
    REQUIRE(w.add("Fifth", Layout::default_build()).has_value());
    REQUIRE(w.set_active(builtins + 1));
    const std::string active_name = w.items()[w.active()].name;
    REQUIRE(w.remove(builtins).has_value());
    CHECK_MSG(w.items()[w.active()].name == active_name,
              "removing an earlier workspace moved which one was active");
}

LTEST_CASE(a_set_survives_a_round_trip_including_which_one_is_active) {
    Workspaces w = Workspaces::defaults();
    REQUIRE(w.add("Editing", Layout::default_build()).has_value());
    Layout live = w.items()[1].layout;
    REQUIRE(live.close_panel("outputs"));
    REQUIRE(w.set_layout(1, live).has_value());
    REQUIRE(w.set_active(1));

    const std::string text = to_toml(w);
    const auto back = workspaces_from_toml(text);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK_EQ(names_of(*back), names_of(w));
    CHECK_EQ(back->active(), std::size_t{1});
    CHECK_MSG(!back->items()[1].layout.has_panel("outputs"),
              "a hidden panel came back visible");
    CHECK_MSG(back->items()[w.size() - 1].builtin == false,
              "an added workspace came back built in");

    // Writing it again produces the same bytes, which is what makes the file diffable.
    CHECK_EQ(to_toml(*back), text);
}

LTEST_CASE(the_file_holds_no_pixel_geometry) {
    // ADR-0015 rule 4, asserted for the workspace file as well as the layout one: a saved
    // arrangement that remembered pixels would be the OBS failure this project exists to avoid.
    Workspaces w = Workspaces::defaults();

    // The comment header explains that no pixels are stored, so it says "pixel" and "monitor"
    // itself. What matters is the data, so the comments are stripped before looking.
    std::string data;
    for (const std::string& line : split_lines(to_toml(w))) {
        if (!line.empty() && line.front() != '#')
            data += line + "\n";
    }

    for (const char* banned : {"width", "height", " x =", " y =", "pixel", "geometry", "screen",
                               "monitor"}) {
        CHECK_MSG(data.find(banned) == std::string::npos, "the file mentions '{}'", banned);
    }
}

LTEST_CASE(an_older_single_layout_file_is_adopted_rather_than_discarded) {
    // Someone who had arranged their workspace before this existed should not lose it.
    Layout mine = Layout::default_build();
    REQUIRE(mine.close_panel("log"));
    const std::string old_format = to_toml(mine);
    CHECK(old_format.find("[[workspace]]") == std::string::npos);

    const auto back = workspaces_from_toml(old_format);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    // Build comes from the file; the rest are backfilled, because a file written before they
    // existed must not cost the operator the arrangement it does hold.
    CHECK_EQ(names_of(*back), (std::vector<std::string>{"Build", "Live", "Encode", "Media",
                                                        "Edit", "Graphics"}));
    CHECK_MSG(!back->items()[0].layout.has_panel("log"),
              "the arrangement from the older file was not adopted");
    CHECK_MSG(back->items()[1].layout.has_panel("broadcast-stats"),
              "Live did not come up as its default");
}

LTEST_CASE(a_file_that_cannot_be_trusted_falls_back_and_says_so) {
    for (const char* bad : {
             "",
             "version = 99\n",
             "version = 1\n",                               // no workspaces, no layout either
             "version = 1\n[[workspace]]\nname = \"\"\n",   // nameless
             "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"wat\"\n",
             "this is not toml at all [[[",
         }) {
        const auto r = workspaces_from_toml(bad);
        CHECK_MSG(!r.has_value(), "accepted a file this build should refuse: {}", bad);
    }
}

LTEST_CASE(a_file_claiming_a_built_in_is_removable_is_not_believed) {
    // `builtin` is taken from the name on load, never from the file. Otherwise a hand-edited
    // file could mark everything removable and leave no known arrangement to get back to.
    Workspaces w = Workspaces::defaults();
    std::string text = to_toml(w);
    const std::size_t at = text.find("builtin = true");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string{"builtin = true"}.size(), "builtin = false");

    const auto back = workspaces_from_toml(text);
    REQUIRE(back.has_value());
    CHECK_MSG(back->items()[0].builtin, "a built-in workspace was demoted by the file");
    CHECK(!back->validate().has_value());
}

LTEST_CASE(a_set_missing_a_built_in_is_refused) {
    // Added one at a time: every built-in is required, so the set is invalid until the last
    // one is in. Driven off the real list rather than a copy of it, so adding a built-in
    // cannot leave this test passing while it checks fewer than there are.
    const std::vector<std::pair<std::string, Layout>> builtins{
        {"Build", Layout::default_build()},   {"Live", Layout::default_live()},
        {"Encode", Layout::default_build()},  {"Media", Layout::default_media()},
        {"Edit", Layout::default_edit()},     {"Graphics", Layout::default_graphics()},
    };

    Workspaces w;
    for (std::size_t i = 0; i < builtins.size(); ++i) {
        const auto added = w.add(builtins[i].first, builtins[i].second);
        REQUIRE(added.has_value());
        w.make_builtin(*added);
        const bool last = i + 1 == builtins.size();
        CHECK_MSG(w.validate().has_value() != last,
                  "a set holding {} of {} built-ins was judged wrongly", i + 1,
                  builtins.size());
    }
}

LTEST_CASE(a_file_written_before_a_built_in_existed_keeps_what_it_does_hold) {
    // The hazard this guards: requiring a new built-in without backfilling rejects every file
    // on disk, and a rejected file takes the operator's own workspaces with it.
    Workspaces w = Workspaces::defaults();
    REQUIRE(w.add("Editing", Layout::default_build()).has_value());
    std::string text = to_toml(w);

    // Cut Encode back out, leaving the file an older build would have written.
    const std::size_t at = text.find("[[workspace]]\nname = \"Encode\"");
    REQUIRE(at != std::string::npos);
    const std::size_t next = text.find("[[workspace]]", at + 1);
    REQUIRE(next != std::string::npos);
    text.erase(at, next - at);
    REQUIRE(text.find("\"Encode\"") == std::string::npos);

    const auto back = workspaces_from_toml(text);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    // Everything the file held, in the order it held it, and only the cut-out built-in
    // appended: a backfill must not reorder what the operator already had.
    CHECK_MSG(names_of(*back) == (std::vector<std::string>{"Build", "Live", "Media", "Edit",
                                                           "Graphics", "Editing", "Encode"}),
              "the backfilled built-in did not land after what the file held");
    const auto editing = std::ranges::find(back->items(), "Editing", &Workspace::name);
    REQUIRE(editing != back->items().end());
    CHECK_MSG(!editing->builtin, "the operator's own workspace was lost or demoted");
}

LTEST_MAIN("workspaces")
