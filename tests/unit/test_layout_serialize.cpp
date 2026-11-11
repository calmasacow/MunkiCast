// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout persistence: ADR-0015 rule 4 (proportions only) and rule 5 (atomic, versioned, and it
// says so when it falls back).

#include "llayout/serialize.hpp"

#include "lcore/atomic_file.hpp"
#include "ltest.hpp"

#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <unistd.h>

using namespace llayout;

namespace {

Viewport vp_of(double w, double h) {
    Viewport vp;
    vp.bounds = Rect{0.0, 0.0, w, h};
    return vp;
}

/// A directory of our own, removed on destruction, so a failing run cannot poison the next one.
class ScratchDir {
public:
    ScratchDir() {
        path_ = std::filesystem::temp_directory_path() /
                std::format("munkicast-llayout-{}", ::getpid());
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] std::filesystem::path file(std::string_view name) const { return path_ / name; }
    [[nodiscard]] const std::filesystem::path& dir() const { return path_; }

private:
    std::filesystem::path path_;
};

std::size_t count_entries(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         it != std::filesystem::directory_iterator(); ++it)
        ++n;
    return n;
}

} // namespace

LTEST_CASE(the_default_layout_survives_a_round_trip) {
    const Layout l = Layout::default_studio();
    const std::string text = to_toml(l);

    const auto back = from_toml(text);
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK(*back == l);
    CHECK(!back->validate().has_value());
    CHECK_EQ(back->panels(), l.panels());

    // Serialisation is a fixed point: writing what we read gives the same bytes.
    CHECK_EQ(to_toml(*back), text);
}

LTEST_CASE(a_mutated_layout_survives_a_round_trip_including_divider_positions) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);

    Layout l = Layout::default_studio();
    REQUIRE(l.dock("chat", l.node_of_panel("program"), Zone::Bottom, vp, cs).applied());
    REQUIRE(l.dock("log", l.node_of_panel("audio-mixer"), Zone::Center, vp, cs).applied());
    REQUIRE(l.dock_edge("browser", Edge::Right, vp, cs).applied());
    const NodeId split = l.find(l.node_of_panel("program"))->parent;
    REQUIRE(l.move_divider(split, 0, 37.5, vp, cs));

    const auto back = from_toml(to_toml(l));
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK(*back == l);

    // Geometry is what actually has to survive, not the node numbering.
    const auto a = l.leaves(vp, cs);
    const auto b = back->leaves(vp, cs);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK_NEAR(a[i].rect.x, b[i].rect.x, 1e-6);
        CHECK_NEAR(a[i].rect.y, b[i].rect.y, 1e-6);
        CHECK_NEAR(a[i].rect.w, b[i].rect.w, 1e-6);
        CHECK_NEAR(a[i].rect.h, b[i].rect.h, 1e-6);
    }
}

LTEST_CASE(the_file_holds_no_pixel_geometry) {
    // ADR-0015 rule 4. If a pixel count ever leaks into this file, a layout saved on a 1440p
    // triple-head restores wrong on a laptop -- which is the exact failure this project exists
    // to stop repeating.
    // Only the data matters; the header comment is free to use whatever words it likes.
    std::string data;
    std::istringstream in{to_toml(Layout::default_studio())};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] != '#')
            data += line + "\n";
    }
    for (std::string_view banned : {"\"x\"", "\"y\"", "width", "height", "px", "geometry"})
        CHECK_MSG(data.find(banned) == std::string::npos, "the layout file mentions {}", banned);
}

LTEST_CASE(a_tabbed_group_remembers_which_tab_was_visible) {
    Layout l = Layout::default_studio();
    REQUIRE(l.activate("log"));
    const auto back = from_toml(to_toml(l));
    REQUIRE(back.has_value());
    CHECK_EQ(back->find(back->node_of_panel("log"))->active, std::size_t{2});
}

LTEST_CASE(awkward_panel_ids_round_trip) {
    const Constraints cs = Constraints::studio_defaults();
    const Viewport vp = vp_of(2560.0, 1440.0);
    Layout l;
    REQUIRE(l.dock("panel with spaces", l.root(), Zone::Center, vp, cs).applied());
    REQUIRE(l.dock("quote\"and\\slash", l.node_of_panel("panel with spaces"), Zone::Right, vp, cs)
                .applied());

    const auto back = from_toml(to_toml(l));
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK(back->has_panel("panel with spaces"));
    CHECK(back->has_panel("quote\"and\\slash"));
    CHECK(*back == l);
}

LTEST_CASE(malformed_files_are_rejected_with_a_reason) {
    struct Bad {
        const char* why;
        const char* text;
    };
    const Bad cases[] = {
        {"not toml at all", "this is not { toml"},
        {"no version", "root = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"future version",
         "version = 99\nroot = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"no root", "version = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"root is not a node",
         "version = 1\nroot = 7\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"no nodes", "version = 1\nroot = 1\n"},
        {"unknown kind",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"grid\"\npanels = [\"a\"]\n"},
        {"bad axis",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"diagonal\"\n"
         "children = [2, 3]\nfractions = [0.5, 0.5]\n"},
        {"children without fractions",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
         "children = [2, 3]\n"},
        {"fraction count mismatch",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
         "children = [2, 3]\nfractions = [0.5]\n"},
        {"duplicate node id",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"
         "[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"b\"]\n"},
        {"missing child",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
         "children = [2, 9]\nfractions = [0.5, 0.5]\n"
         "[[node]]\nid = 2\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"a cycle",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
         "children = [2, 3]\nfractions = [0.5, 0.5]\n"
         "[[node]]\nid = 2\nkind = \"split\"\naxis = \"v\"\nchildren = [1, 3]\n"
         "fractions = [0.5, 0.5]\n"
         "[[node]]\nid = 3\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"an unreachable node",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"a\"]\n"
         "[[node]]\nid = 4\nkind = \"tabs\"\npanels = [\"b\"]\n"},
        {"the same panel twice",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
         "children = [2, 3]\nfractions = [0.5, 0.5]\n"
         "[[node]]\nid = 2\nkind = \"tabs\"\npanels = [\"a\"]\n"
         "[[node]]\nid = 3\nkind = \"tabs\"\npanels = [\"a\"]\n"},
        {"an empty panel id",
         "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"tabs\"\npanels = [\"\"]\n"},
        {"a zero node id",
         "version = 1\nroot = 1\n[[node]]\nid = 0\nkind = \"tabs\"\npanels = [\"a\"]\n"},
    };

    for (const Bad& c : cases) {
        const auto r = from_toml(c.text);
        CHECK_MSG(!r.has_value(), "accepted a layout that is {}", c.why);
        if (!r.has_value())
            CHECK_MSG(!r.error().empty(), "rejected '{}' with an empty reason", c.why);
    }
}

LTEST_CASE(a_split_with_lopsided_fractions_is_repaired_not_rejected) {
    // Fractions are the one field where a hand-edit is likely and harmless: normalising them is
    // strictly better than throwing away the user's whole layout over arithmetic.
    const auto r = from_toml("version = 1\nroot = 1\n"
                             "[[node]]\nid = 1\nkind = \"split\"\naxis = \"h\"\n"
                             "children = [2, 3]\nfractions = [3.0, 1.0]\n"
                             "[[node]]\nid = 2\nkind = \"tabs\"\npanels = [\"a\"]\n"
                             "[[node]]\nid = 3\nkind = \"tabs\"\npanels = [\"b\"]\n");
    REQUIRE_MSG(r.has_value(), "{}", r.error());
    CHECK(!r->validate().has_value());
    const Node* root = r->find(r->root());
    REQUIRE(root != nullptr);
    CHECK_NEAR(root->fractions[0], 0.75, 1e-9);
    CHECK_NEAR(root->fractions[1], 0.25, 1e-9);
}

LTEST_CASE(saving_and_loading_leaves_no_litter) {
    const ScratchDir sd;
    const std::filesystem::path path = sd.file("layout.toml");

    const Layout l = Layout::default_studio();
    const auto saved = save(l, path);
    REQUIRE_MSG(saved.has_value(), "{}", saved.error());
    CHECK(std::filesystem::exists(path));
    // The temporary must be gone: a stray .tmp-* beside a config file is how a config directory
    // stops being trustworthy.
    CHECK_EQ(count_entries(sd.dir()), std::size_t{1});

    const auto loaded = load(path);
    REQUIRE_MSG(loaded.has_value(), "{}", loaded.error());
    CHECK(*loaded == l);

    // Overwriting works, and still leaves one file.
    Layout l2 = l;
    CHECK(l2.close_panel("log"));
    REQUIRE(save(l2, path).has_value());
    CHECK_EQ(count_entries(sd.dir()), std::size_t{1});
    const auto reloaded = load(path);
    REQUIRE(reloaded.has_value());
    CHECK(*reloaded == l2);
    CHECK(!reloaded->has_panel("log"));
}

LTEST_CASE(write_atomic_writes_exactly_what_it_was_given) {
    const ScratchDir sd;
    const std::filesystem::path path = sd.file("nested/dir/data.txt");
    const std::string payload("line one\nline\0two\n", 18);
    const std::string_view data{payload.data(), payload.size()};

    REQUIRE(lc::write_atomic(path, data).has_value());
    const auto back = lc::read_file(path);
    REQUIRE(back.has_value());
    CHECK_EQ(back->size(), data.size());
    CHECK(*back == std::string(data));

    // A shorter payload must truncate, not leave the tail of the previous one behind.
    REQUIRE(lc::write_atomic(path, "short").has_value());
    const auto second = lc::read_file(path);
    REQUIRE(second.has_value());
    CHECK_EQ(*second, std::string("short"));
}

LTEST_CASE(reading_a_file_that_is_not_there_fails_rather_than_returning_nothing) {
    const ScratchDir sd;
    const auto r = lc::read_file(sd.file("absent"));
    CHECK(!r.has_value());
    CHECK(!r.error().empty());
}

LTEST_CASE(a_missing_layout_file_falls_back_to_the_default_silently) {
    const ScratchDir sd;
    const LoadOutcome o = load_or_default(sd.file("never-written.toml"));
    CHECK(o.used_default);
    CHECK(o.complaint.empty()); // nothing went wrong; there was simply nothing there yet
    CHECK(o.layout == Layout::default_studio());
}

LTEST_CASE(a_broken_layout_file_falls_back_to_the_default_and_says_so) {
    // ADR-0015 rule 5. "Randomly rearranging things" is what a torn layout file looks like on the
    // next launch, and a silent fallback is indistinguishable from the bug.
    const ScratchDir sd;
    const std::filesystem::path path = sd.file("layout.toml");
    REQUIRE(lc::write_atomic(path, "version = 1\nroot = 1\n[[node]]\nid = 1\nkind = \"nope\"\n")
                .has_value());

    const LoadOutcome o = load_or_default(path);
    CHECK(o.used_default);
    CHECK_MSG(!o.complaint.empty(), "fell back without saying why");
    CHECK(o.layout == Layout::default_studio());
    CHECK(!o.layout.validate().has_value());
}

LTEST_CASE(a_truncated_layout_file_falls_back_to_the_default_and_says_so) {
    const ScratchDir sd;
    const std::filesystem::path path = sd.file("layout.toml");
    const std::string whole = to_toml(Layout::default_studio());
    REQUIRE(lc::write_atomic(path, std::string_view{whole}.substr(0, whole.size() / 2))
                .has_value());

    const LoadOutcome o = load_or_default(path);
    CHECK(o.used_default);
    CHECK(!o.complaint.empty());
}

LTEST_CASE(a_version_one_file_loses_the_panel_this_build_dropped) {
    // The `inputs` panel is gone: a source is a source, and listing the project's sources moved
    // into the Sources panel's add menu. A version 1 layout still names it, and without the
    // migration it would come back as a panel this build has nothing to draw in -- visible,
    // confusing, and permanent until the user closed it by hand.
    const std::string v1 =
        "version = 1\n"
        "root = 1\n"
        "\n[[node]]\n"
        "id = 1\n"
        "kind = \"split\"\n"
        "axis = \"h\"\n"
        "children = [ 2, 3 ]\n"
        "fractions = [ 0.5, 0.5 ]\n"
        "\n[[node]]\n"
        "id = 2\n"
        "kind = \"tabs\"\n"
        "panels = [ \"program\" ]\n"
        "active = 0\n"
        "\n[[node]]\n"
        "id = 3\n"
        "kind = \"tabs\"\n"
        "panels = [ \"properties\", \"inputs\", \"log\" ]\n"
        "active = 1\n";

    const auto back = from_toml(v1);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK_MSG(!back->has_panel("inputs"), "a dropped panel survived the migration");
    CHECK_MSG(back->has_panel("properties") && back->has_panel("log") && back->has_panel("program"),
              "the migration took panels it should have kept");
    CHECK(!back->validate().has_value());

    // The tab that was visible was the one removed, so the selection has to land somewhere real
    // rather than past the end.
    const Node* group = back->find(back->node_of_panel("properties"));
    REQUIRE(group != nullptr);
    CHECK_EQ(group->tabs.size(), std::size_t{2});
    CHECK_MSG(group->active < group->tabs.size(), "the visible tab index is past the end");

    // Written back out, it is the current version and says nothing about the dropped panel.
    const std::string again = to_toml(*back);
    CHECK(again.find(std::format("version = {}", kLayoutFormatVersion)) != std::string::npos);
    CHECK(again.find("inputs") == std::string::npos);
}

LTEST_CASE(a_version_two_file_renames_stats_to_info) {
    // The panel called "Stats" is the panel now called "Info" -- it counts what is in the
    // project, which was never a statistic. The rename shipped with no migration, so a saved
    // layout kept naming `stats` and the shell drew its "nothing to draw there" placeholder in
    // that tab for ever. Version 2 files are affected too, which is why the gate is `< 3`.
    const std::string v2 =
        "version = 2\n"
        "root = 1\n"
        "\n[[node]]\n"
        "id = 1\n"
        "kind = \"split\"\n"
        "axis = \"h\"\n"
        "children = [ 2, 3 ]\n"
        "fractions = [ 0.5, 0.5 ]\n"
        "\n[[node]]\n"
        "id = 2\n"
        "kind = \"tabs\"\n"
        "panels = [ \"program\" ]\n"
        "active = 0\n"
        "\n[[node]]\n"
        "id = 3\n"
        "kind = \"tabs\"\n"
        "panels = [ \"stats\", \"log\" ]\n"
        "active = 0\n";

    const auto back = from_toml(v2);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK_MSG(!back->has_panel("stats"), "the retired name survived the migration");
    CHECK_MSG(back->has_panel("info"), "the panel was dropped instead of renamed");
    CHECK_MSG(back->has_panel("log") && back->has_panel("program"),
              "the migration took panels it should have kept");
    CHECK(!back->validate().has_value());

    // A rename, so the group keeps both tabs and the visible one does not move.
    const Node* group = back->find(back->node_of_panel("info"));
    REQUIRE(group != nullptr);
    CHECK_EQ(group->tabs.size(), std::size_t{2});
    CHECK_EQ(group->tabs.front(), std::string{"info"});
    CHECK_EQ(group->active, std::size_t{0});

    const std::string again = to_toml(*back);
    CHECK(again.find("stats") == std::string::npos);
}

LTEST_CASE(a_file_with_both_stats_and_info_keeps_only_info) {
    // Renaming here would put one panel in the layout twice, so the stale tab is closed
    // instead. The group it was in holds `info` already, so nothing collapses.
    const std::string v2 =
        "version = 2\n"
        "root = 1\n"
        "\n[[node]]\n"
        "id = 1\n"
        "kind = \"split\"\n"
        "axis = \"h\"\n"
        "children = [ 2, 3 ]\n"
        "fractions = [ 0.5, 0.5 ]\n"
        "\n[[node]]\n"
        "id = 2\n"
        "kind = \"tabs\"\n"
        "panels = [ \"program\" ]\n"
        "active = 0\n"
        "\n[[node]]\n"
        "id = 3\n"
        "kind = \"tabs\"\n"
        "panels = [ \"info\", \"stats\", \"log\" ]\n"
        "active = 1\n";

    const auto back = from_toml(v2);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK_MSG(!back->has_panel("stats"), "the stale duplicate survived");
    CHECK_MSG(back->has_panel("info"), "the real panel was removed instead of the stale one");
    CHECK(!back->validate().has_value());

    const Node* group = back->find(back->node_of_panel("info"));
    REQUIRE(group != nullptr);
    CHECK_EQ(group->tabs.size(), std::size_t{2});
    CHECK_MSG(group->active < group->tabs.size(), "the visible tab index is past the end");
}

LTEST_CASE(a_group_of_only_stats_becomes_a_group_of_only_info) {
    // The rename must not collapse the group the way the `inputs` removal does: the panel is
    // still there, under the name this build knows it by.
    const std::string v2 =
        "version = 2\n"
        "root = 1\n"
        "\n[[node]]\n"
        "id = 1\n"
        "kind = \"split\"\n"
        "axis = \"h\"\n"
        "children = [ 2, 3 ]\n"
        "fractions = [ 0.5, 0.5 ]\n"
        "\n[[node]]\n"
        "id = 2\n"
        "kind = \"tabs\"\n"
        "panels = [ \"program\" ]\n"
        "active = 0\n"
        "\n[[node]]\n"
        "id = 3\n"
        "kind = \"tabs\"\n"
        "panels = [ \"stats\" ]\n"
        "active = 0\n";

    const auto back = from_toml(v2);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK_MSG(back->has_panel("info"), "the only panel in the group was lost");
    CHECK(!back->has_panel("stats"));
    CHECK(!back->validate().has_value());
}

LTEST_CASE(a_version_one_group_of_only_the_dropped_panel_collapses) {
    // The harder case: the migration empties a whole tab group. It must collapse rather than
    // leave an empty leaf, which validate() would reject.
    const std::string v1 =
        "version = 1\n"
        "root = 1\n"
        "\n[[node]]\n"
        "id = 1\n"
        "kind = \"split\"\n"
        "axis = \"h\"\n"
        "children = [ 2, 3 ]\n"
        "fractions = [ 0.5, 0.5 ]\n"
        "\n[[node]]\n"
        "id = 2\n"
        "kind = \"tabs\"\n"
        "panels = [ \"program\" ]\n"
        "active = 0\n"
        "\n[[node]]\n"
        "id = 3\n"
        "kind = \"tabs\"\n"
        "panels = [ \"inputs\" ]\n"
        "active = 0\n";

    const auto back = from_toml(v1);
    REQUIRE_MSG(back.has_value(), "{}", back ? std::string{} : back.error());
    CHECK(!back->has_panel("inputs"));
    CHECK_MSG(!back->validate().has_value(), "{}", back->validate().value_or(""));
    CHECK_EQ(back->panels(), (std::vector<std::string>{"program"}));
}

LTEST_CASE(a_file_from_a_newer_build_is_still_refused) {
    // Migration forward only. A file this build cannot understand must not be guessed at.
    const auto newer = from_toml("version = 99\nroot = 1\n");
    CHECK(!newer.has_value());
    CHECK(newer.error().contains("99"));
}

LTEST_MAIN("layout-serialize")
