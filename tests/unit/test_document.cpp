// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The document model: structure, integrity and -- the one the plan calls out by name -- copy and
// id hygiene.

#include "ldoc/document.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <format>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ldoc;

namespace {

std::string errors_of(const Project& p) {
    std::string out;
    for (const Problem& problem : validate(p)) {
        if (problem.severity != Severity::Error)
            continue;
        if (!out.empty())
            out += "; ";
        out += problem.message;
    }
    return out;
}

std::string warnings_of(const Project& p) {
    std::string out;
    for (const Problem& problem : validate(p)) {
        if (problem.severity != Severity::Warning)
            continue;
        if (!out.empty())
            out += "; ";
        out += problem.message;
    }
    return out;
}

std::size_t warning_count(const Project& p) {
    const auto problems = validate(p);
    return static_cast<std::size_t>(std::count_if(
        problems.begin(), problems.end(),
        [](const Problem& x) { return x.severity == Severity::Warning; }));
}

std::size_t error_count(const Project& p) {
    const auto problems = validate(p);
    return static_cast<std::size_t>(std::count_if(
        problems.begin(), problems.end(),
        [](const Problem& x) { return x.severity == Severity::Error; }));
}

Node source_node(std::string name) { return make_node(NodeKind::Source, std::move(name)); }
Node group(std::string name) { return make_node(NodeKind::Group, std::move(name)); }

void add_child(Node& parent, Node child) {
    parent.children()->push_back(std::move(child));
}

/// Everything about a node except the ids, so two trees can be compared for shape.
bool same_shape(const Node& a, const Node& b) {
    if (a.name != b.name || a.visible != b.visible || a.locked != b.locked)
        return false;
    if (!(a.transform == b.transform) || !(a.layout == b.layout) || !(a.blend == b.blend))
        return false;
    if (a.kind() != b.kind() || a.effects.size() != b.effects.size())
        return false;
    for (std::size_t i = 0; i < a.effects.size(); ++i) {
        if (a.effects[i].type != b.effects[i].type ||
            a.effects[i].enabled != b.effects[i].enabled ||
            a.effects[i].params != b.effects[i].params)
            return false;
    }
    const auto* ka = a.children();
    const auto* kb = b.children();
    if ((ka == nullptr) != (kb == nullptr))
        return false;
    if (ka) {
        if (ka->size() != kb->size())
            return false;
        for (std::size_t i = 0; i < ka->size(); ++i)
            if (!same_shape((*ka)[i], (*kb)[i]))
                return false;
    }
    return true;
}

/// A scene tree with groups, source nodes, effects and references, built from a fixed seed so a
/// failure is reproducible.
std::vector<Node> random_tree(std::mt19937& rng, int depth, const Id& outside_target) {
    std::vector<Node> out;
    const int count = 1 + static_cast<int>(rng() % 3);
    for (int i = 0; i < count; ++i) {
        const auto roll = rng() % 100;
        Node n;
        if (depth > 0 && roll < 35) {
            n = group(std::format("group-{}", rng() % 1000));
            for (Node& c : random_tree(rng, depth - 1, outside_target))
                add_child(n, std::move(c));
        } else if (roll < 55) {
            n = make_node(NodeKind::Reference, std::format("ref-{}", rng() % 1000));
            // Filled in by the caller once every node exists; a nil target here would be an
            // invalid document rather than an interesting one.
            std::get<ReferenceData>(n.data).target = outside_target;
        } else if (roll < 65) {
            n = make_node(NodeKind::Adjustment, "adjust");
        } else if (roll < 72) {
            n = make_node(NodeKind::Mask, "mask");
        } else {
            n = source_node(std::format("source-{}", rng() % 1000));
        }

        n.blend.opacity = static_cast<double>(rng() % 101) / 100.0;
        if (rng() % 3 == 0) {
            Effect e;
            e.id = Id::generate();
            e.type = "effect.blur";
            e.params.emplace("radius", static_cast<double>(rng() % 50));
            n.effects.push_back(std::move(e));
        }
        out.push_back(std::move(n));
    }
    return out;
}

} // namespace

LTEST_CASE(a_new_project_is_valid_and_minimal) {
    const Project p = make_default_project("Test Show");
    CHECK_MSG(is_valid(p), "{}", errors_of(p));
    CHECK_EQ(p.schema_version, kSchemaVersion);
    CHECK_EQ(p.canvases.size(), std::size_t{1});
    CHECK_EQ(p.scenes.size(), std::size_t{1});
    CHECK_EQ(p.decks.size(), std::size_t{1});
    CHECK_EQ(p.canvases[0].fps.hz(), 60.0);

    // The VOD track exists from the start, because adding it later means re-routing every source
    // that already exists.
    CHECK(std::any_of(p.buses.begin(), p.buses.end(),
                      [](const Bus& b) { return b.kind == BusKind::Vod; }));
    CHECK(std::any_of(p.buses.begin(), p.buses.end(),
                      [](const Bus& b) { return b.kind == BusKind::Program; }));
    // Nothing pretends to be configured.
    CHECK(p.sources.empty());
    CHECK(p.outputs.empty());
    CHECK(p.scenes[0].roots.empty());
}

LTEST_CASE(a_frame_rate_is_an_exact_rational) {
    // 59.94 is 60000/1001 and nothing else. A pipeline that rounds it drifts, and the drift
    // shows up hours into a broadcast as audio sync.
    const Fps ntsc = Fps::ntsc(60);
    CHECK_EQ(ntsc.num, 60000);
    CHECK_EQ(ntsc.den, 1001);
    CHECK_NEAR(ntsc.hz(), 59.940059940, 1e-9);
    CHECK(ntsc.valid());
    CHECK(!(Fps{30, 0}.valid()));
    CHECK(!(Fps{0, 1}.valid()));
    CHECK(!(Fps{-30, 1}.valid()));
}

LTEST_CASE(colours_are_stored_in_linear_light) {
    // Mid-grey in sRGB is a long way from 0.5 in linear light. Storing the sRGB value would make
    // every blend subtly wrong in a way that reads as "too dark", not as a bug.
    const Rgba grey = Rgba::from_srgb8(128, 128, 128);
    CHECK(grey.r < 0.25);
    CHECK_NEAR(grey.r, 0.2158605, 1e-6);
    CHECK_EQ(grey.a, 1.0);

    CHECK_NEAR(Rgba::from_srgb8(0, 0, 0).r, 0.0, 1e-12);
    CHECK_NEAR(Rgba::from_srgb8(255, 255, 255).r, 1.0, 1e-12);

    // And the boundary conversion round-trips for every 8-bit value.
    for (int v = 0; v <= 255; ++v) {
        const auto back = Rgba::from_srgb8(static_cast<std::uint8_t>(v),
                                           static_cast<std::uint8_t>(v),
                                           static_cast<std::uint8_t>(v))
                              .to_srgb8();
        CHECK_MSG(back[0] == v, "sRGB {} came back as {}", v, static_cast<int>(back[0]));
    }
}

LTEST_CASE(the_four_non_separable_blend_modes_need_the_backdrop) {
    // Not a rendering detail to be discovered later: a subtree containing one of these must be
    // flattened to an intermediate, so the planner needs to know from day one.
    for (const BlendMode m : {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color,
                              BlendMode::Luminosity})
        CHECK(needs_backdrop(m));
    for (const BlendMode m : {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen,
                              BlendMode::Overlay, BlendMode::Difference, BlendMode::HardMix})
        CHECK(!needs_backdrop(m));
}

LTEST_CASE(enum_names_round_trip) {
    // The names are the on-disk format, so a typo here is a format bug.
    for (int i = 0; i < 25; ++i) {
        const auto m = static_cast<BlendMode>(i);
        const auto back = blend_mode_from_string(to_string(m));
        REQUIRE_MSG(back.has_value(), "blend mode {} has no name", i);
        CHECK(*back == m);
    }
    for (int i = 0; i < 6; ++i) {
        const auto k = static_cast<NodeKind>(i);
        CHECK(node_kind_from_string(to_string(k)).value() == k);
    }
    for (int i = 0; i < 6; ++i) {
        const auto k = static_cast<BusKind>(i);
        CHECK(bus_kind_from_string(to_string(k)).value() == k);
    }
    for (int i = 0; i < 6; ++i) {
        const auto f = static_cast<FitMode>(i);
        CHECK(fit_mode_from_string(to_string(f)).value() == f);
    }
    for (int i = 0; i < 5; ++i) {
        const auto c = static_cast<ColorSpace>(i);
        CHECK(color_space_from_string(to_string(c)).value() == c);
    }
    CHECK(!blend_mode_from_string("not-a-mode").has_value());
}

LTEST_CASE(only_a_group_can_hold_children) {
    // The kind-specific payload is a variant so that a source node with children is not representable.
    // A validator that has to reject such a thing is a validator that will be run too late.
    Node g = group("g");
    REQUIRE(g.children() != nullptr);
    add_child(g, source_node("a"));
    CHECK_EQ(g.children()->size(), std::size_t{1});

    for (const NodeKind k : {NodeKind::Source, NodeKind::Adjustment, NodeKind::Mask,
                             NodeKind::Reference, NodeKind::Precomp}) {
        const Node n = make_node(k, "x");
        CHECK_MSG(n.children() == nullptr, "{} reports children", to_string(k));
        CHECK(n.kind() == k);
    }
}

LTEST_CASE(traversal_is_depth_first_with_the_topmost_node_first) {
    // roots[0] is what the Sources panel shows at the top. If this order ever flips, every piece of
    // UI has to reverse the list and one of them will forget.
    Node top = group("top");
    add_child(top, source_node("top-a"));
    add_child(top, source_node("top-b"));
    Node bottom = source_node("bottom");

    std::vector<Node> roots;
    roots.push_back(std::move(top));
    roots.push_back(std::move(bottom));

    std::vector<std::string> seen;
    visit(roots, [&seen](const Node& n) { seen.push_back(n.name); });
    CHECK_EQ(seen, (std::vector<std::string>{"top", "top-a", "top-b", "bottom"}));

    CHECK(find_node(roots, roots[0].children()->at(1).id) != nullptr);
    CHECK_EQ(find_node(roots, roots[0].children()->at(1).id)->name, std::string("top-b"));
    CHECK(find_node(roots, Id::generate()) == nullptr);
}

LTEST_CASE(duplication_mints_new_ids_for_everything_it_copies) {
    Node g = group("g");
    Effect e;
    e.id = Id::generate();
    e.type = "effect.stroke";
    g.effects.push_back(e);
    add_child(g, source_node("a"));
    add_child(g, source_node("b"));

    std::vector<Id> original;
    collect_defined_ids({g}, original);
    REQUIRE(original.size() == 4); // the group, its effect, and two source nodes

    std::map<Id, Id> remap;
    const Node copy = duplicate(g, &remap);

    std::vector<Id> copied;
    collect_defined_ids({copy}, copied);
    CHECK_EQ(copied.size(), original.size());

    const std::set<Id> before(original.begin(), original.end());
    for (const Id& id : copied)
        CHECK_MSG(!before.contains(id), "id {} survived the copy", id.to_string());
    CHECK_EQ(remap.size(), original.size());
    CHECK(same_shape(g, copy));
}

LTEST_CASE(duplication_rewrites_inside_references_and_preserves_outside_ones) {
    // Both halves matter. A group containing a Reference to one of its own members should give a
    // group referring to the *copy*; a Reference pointing at something elsewhere in the scene
    // should still point at that same thing.
    Node outside = source_node("outside");
    const Id outside_id = outside.id;

    Node g = group("g");
    add_child(g, source_node("inside"));
    const Id inside_id = g.children()->at(0).id;

    Node ref_in = make_node(NodeKind::Reference, "ref-inside");
    std::get<ReferenceData>(ref_in.data).target = inside_id;
    add_child(g, std::move(ref_in));

    Node ref_out = make_node(NodeKind::Reference, "ref-outside");
    std::get<ReferenceData>(ref_out.data).target = outside_id;
    add_child(g, std::move(ref_out));

    const Node copy = duplicate(g);
    const Id copied_inside = copy.children()->at(0).id;
    const Id ref_in_target = std::get<ReferenceData>(copy.children()->at(1).data).target;
    const Id ref_out_target = std::get<ReferenceData>(copy.children()->at(2).data).target;

    CHECK_MSG(ref_in_target == copied_inside,
              "a reference inside the copy still points at the original");
    CHECK(ref_in_target != inside_id);
    CHECK_MSG(ref_out_target == outside_id,
              "a reference pointing outside the copy was rewritten");
}

LTEST_CASE(duplication_leaves_project_level_references_alone) {
    // Duplicating a source node that shows a camera must show that camera, not open a second one.
    const Id source_id = Id::generate();
    const Id bus_id = Id::generate();
    const Id scene_id = Id::generate();

    Node l = source_node("cam");
    std::get<SourceData>(l.data).source = source_id;
    std::get<SourceData>(l.data).audio.bus = bus_id;

    Node pre = make_node(NodeKind::Precomp, "pre");
    std::get<PrecompData>(pre.data).scene = scene_id;

    Node g = group("g");
    add_child(g, std::move(l));
    add_child(g, std::move(pre));

    const Node copy = duplicate(g);
    const auto& copied_node = std::get<SourceData>(copy.children()->at(0).data);
    CHECK(copied_node.source.value() == source_id);
    CHECK(copied_node.audio.bus.value() == bus_id);
    CHECK(std::get<PrecompData>(copy.children()->at(1).data).scene == scene_id);
}

LTEST_CASE(an_effect_mask_follows_the_copy_when_the_mask_is_inside_it) {
    Node g = group("g");
    add_child(g, make_node(NodeKind::Mask, "m"));
    const Id mask_id = g.children()->at(0).id;

    Node l = source_node("l");
    Effect e;
    e.id = Id::generate();
    e.type = "effect.blur";
    e.mask = mask_id;
    l.effects.push_back(e);
    add_child(g, std::move(l));

    const Node copy = duplicate(g);
    const Id copied_mask = copy.children()->at(0).id;
    CHECK(copy.children()->at(1).effects[0].mask.value() == copied_mask);

    // And a mask that lives outside the copied subtree is left pointing where it was.
    Node lone = source_node("lone");
    Effect out_e;
    out_e.id = Id::generate();
    out_e.type = "effect.blur";
    out_e.mask = Id::generate();
    const Id external_mask = *out_e.mask;
    lone.effects.push_back(out_e);
    CHECK(duplicate(lone).effects[0].mask.value() == external_mask);
}

LTEST_CASE(copy_and_id_hygiene_holds_for_random_trees) {
    // The property the plan asks for by name, over trees no hand-written case would build.
    std::mt19937 rng{20261012u};

    for (int iteration = 0; iteration < 400; ++iteration) {
        const Id outside_target = Id::generate();
        std::vector<Node> tree = random_tree(rng, 3, outside_target);

        // Point roughly half the references at nodes inside the tree, leaving the rest outside.
        std::vector<Id> inside;
        collect_defined_ids(tree, inside);
        std::vector<Id*> targets;
        visit(tree, [&](Node& n) {
            if (auto* r = std::get_if<ReferenceData>(&n.data))
                targets.push_back(&r->target);
        });
        for (Id* t : targets) {
            if (rng() % 2 == 0 && !inside.empty())
                *t = inside[rng() % inside.size()];
        }

        std::vector<Id> before_ids;
        collect_defined_ids(tree, before_ids);
        const std::set<Id> before(before_ids.begin(), before_ids.end());

        std::map<Id, Id> remap;
        const std::vector<Node> copy = duplicate(tree, &remap);

        // 1. No id is shared between the original and the copy.
        std::vector<Id> after_ids;
        collect_defined_ids(copy, after_ids);
        REQUIRE_MSG(after_ids.size() == before_ids.size(), "iteration {}: {} ids became {}",
                    iteration, before_ids.size(), after_ids.size());
        for (const Id& id : after_ids)
            REQUIRE_MSG(!before.contains(id), "iteration {}: id {} survived", iteration,
                        id.to_string());

        // 2. Ids within the copy are themselves unique.
        const std::set<Id> after(after_ids.begin(), after_ids.end());
        REQUIRE_MSG(after.size() == after_ids.size(), "iteration {}: the copy has duplicate ids",
                    iteration);

        // 3. The shape is identical.
        REQUIRE(copy.size() == tree.size());
        for (std::size_t i = 0; i < copy.size(); ++i)
            REQUIRE_MSG(same_shape(tree[i], copy[i]), "iteration {}: shape changed", iteration);

        // 4. Every reference either points inside the copy (and was rewritten) or outside it
        //    (and was not).
        std::vector<Id> copy_targets;
        visit(copy, [&](const Node& n) {
            if (const auto* r = std::get_if<ReferenceData>(&n.data))
                copy_targets.push_back(r->target);
        });
        for (const Id& t : copy_targets) {
            const bool points_into_copy = after.contains(t);
            const bool points_into_original = before.contains(t);
            REQUIRE_MSG(!points_into_original,
                        "iteration {}: the copy still refers into the original", iteration);
            REQUIRE_MSG(points_into_copy || t == outside_target,
                        "iteration {}: a reference went somewhere unexpected", iteration);
        }
    }
}

LTEST_CASE(duplicating_a_scene_carries_its_audio_routing_with_it) {
    Project p = make_default_project("Show");
    const Id bus_id = p.buses[0].id;

    Scene& s = p.scenes[0];
    s.roots.push_back(source_node("mic"));
    const Id mic_id = s.roots[0].id;
    AudioOverride ov;
    ov.gain_db = -6.0;
    ov.bus = bus_id;
    s.audio_overrides.emplace(mic_id, ov);
    Effect filter;
    filter.id = Id::generate();
    filter.type = "filter.mask";
    filter.mask = mic_id;
    filter.name = "Scene mask";
    s.effects.push_back(filter);

    const Scene copy = duplicate_scene(s, "Show Copy");
    CHECK(copy.id != s.id);
    CHECK_EQ(copy.name, std::string("Show Copy"));
    const Id copied_mic = copy.roots[0].id;
    CHECK(copied_mic != mic_id);
    CHECK_MSG(copy.audio_overrides.contains(copied_mic),
              "the routing was left keyed to the original node, so it applies to nothing");
    CHECK(!copy.audio_overrides.contains(mic_id));
    CHECK(copy.audio_overrides.at(copied_mic).bus.value() == bus_id);
    REQUIRE(copy.effects.size() == 1);
    CHECK(copy.effects.front().id != filter.id);
    CHECK(copy.effects.front().mask == copied_mic);
    CHECK_EQ(copy.effects.front().name, filter.name);
}

LTEST_CASE(validation_reports_every_problem_not_just_the_first) {
    Project p = make_default_project("Show");
    p.canvases[0].width = 0;
    p.buses[0].channels = -1;
    p.decks[0].canvas = Id::generate();

    const auto problems = validate(p);
    CHECK_MSG(error_count(p) >= 3, "only {} errors reported: {}", error_count(p), errors_of(p));
    // And every one names something findable.
    for (const Problem& problem : problems)
        CHECK(!problem.subject.is_nil() || problem.severity == Severity::Warning);
}

LTEST_CASE(a_duplicate_id_anywhere_in_the_project_is_an_error) {
    Project p = make_default_project("Show");
    p.scenes[0].roots.push_back(source_node("a"));
    p.scenes[0].roots.push_back(source_node("b"));
    p.scenes[0].roots[1].id = p.scenes[0].roots[0].id;
    CHECK_EQ(error_count(p), std::size_t{1});
    CHECK(errors_of(p).find("is used by both") != std::string::npos);

    // Across different kinds of thing, too.
    Project q = make_default_project("Show");
    q.buses[1].id = q.canvases[0].id;
    CHECK(!is_valid(q));
}

LTEST_CASE(a_nil_id_is_an_error) {
    Project p = make_default_project("Show");
    p.scenes[0].roots.push_back(source_node("a"));
    p.scenes[0].roots[0].id = Id::nil();
    CHECK(!is_valid(p));
    CHECK(errors_of(p).find("no id") != std::string::npos);
}

LTEST_CASE(a_dangling_reference_is_reported_as_a_warning_not_an_error) {
    // Deliberately not an error. A source node whose source was deleted draws nothing, exactly
    // as one with no source does; the only difference is that this one records a deletion.
    // Refusing to open would mean a source could not be removed while any node used it, which
    // makes ordinary editing a puzzle. Errors are for documents that cannot be rendered at all.
    Project p = make_default_project("Show");
    Node l = source_node("cam");
    std::get<SourceData>(l.data).source = Id::generate(); // no such source
    p.scenes[0].roots.push_back(std::move(l));
    CHECK_MSG(is_valid(p), "{}", errors_of(p));
    CHECK_EQ(warning_count(p), std::size_t{1});
    CHECK(warnings_of(p).find("source") != std::string::npos);

    // Pointing at something that exists but is the wrong kind is caught the same way.
    Project q = make_default_project("Show");
    Node l2 = source_node("cam");
    std::get<SourceData>(l2.data).source = q.canvases[0].id;
    q.scenes[0].roots.push_back(std::move(l2));
    CHECK_MSG(warning_count(q) == 1, "a source node using a canvas as its source went unreported");
}

LTEST_CASE(a_deck_without_a_canvas_is_an_error_but_without_a_scene_is_not) {
    // A deck with no canvas has no output surface at all, which is structural. A deck whose
    // program scene was deleted shows nothing -- which is what a deck with no program scene
    // does anyway.
    Project p = make_default_project("Show");
    p.decks[0].program_scene = Id::generate();
    CHECK_MSG(is_valid(p), "{}", errors_of(p));
    CHECK(warnings_of(p).find("program scene that was removed") != std::string::npos);

    p.decks[0].canvas = Id::generate();
    CHECK_MSG(!is_valid(p), "a deck with no canvas was accepted");
}

LTEST_CASE(a_reference_loop_is_refused) {
    // A loop would make the renderer recurse until it died. Catching it here means the renderer
    // never needs a depth limit whose only purpose is to survive a document we should have
    // refused.
    Project p = make_default_project("Show");
    Node a = make_node(NodeKind::Reference, "a");
    Node b = make_node(NodeKind::Reference, "b");
    std::get<ReferenceData>(a.data).target = b.id;
    std::get<ReferenceData>(b.data).target = a.id;
    p.scenes[0].roots.push_back(std::move(a));
    p.scenes[0].roots.push_back(std::move(b));

    CHECK(!is_valid(p));
    CHECK(errors_of(p).find("loop") != std::string::npos);

    // A reference to itself is the degenerate case and must also be caught.
    Project q = make_default_project("Show");
    Node self = make_node(NodeKind::Reference, "self");
    std::get<ReferenceData>(self.data).target = self.id;
    q.scenes[0].roots.push_back(std::move(self));
    CHECK(!is_valid(q));
}

LTEST_CASE(a_precomp_loop_is_refused) {
    Project p = make_default_project("Show");
    Scene second;
    second.id = Id::generate();
    second.name = "Second";
    second.canvas = p.canvases[0].id;
    p.scenes.push_back(second);

    Node a = make_node(NodeKind::Precomp, "a");
    std::get<PrecompData>(a.data).scene = p.scenes[1].id;
    p.scenes[0].roots.push_back(std::move(a));

    Node b = make_node(NodeKind::Precomp, "b");
    std::get<PrecompData>(b.data).scene = p.scenes[0].id;
    p.scenes[1].roots.push_back(std::move(b));

    CHECK_MSG(!is_valid(p), "two scenes precomposing each other was accepted");
    CHECK(errors_of(p).find("precomposes itself") != std::string::npos);
}

LTEST_CASE(a_precomp_chain_that_is_not_a_loop_is_fine) {
    Project p = make_default_project("Show");
    for (int i = 0; i < 2; ++i) {
        Scene s;
        s.id = Id::generate();
        s.name = std::format("s{}", i);
        s.canvas = p.canvases[0].id;
        p.scenes.push_back(s);
    }
    // 0 -> 1 -> 2, and the same precomp used twice.
    Node a = make_node(NodeKind::Precomp, "a");
    std::get<PrecompData>(a.data).scene = p.scenes[1].id;
    Node a2 = make_node(NodeKind::Precomp, "a2");
    std::get<PrecompData>(a2.data).scene = p.scenes[2].id;
    p.scenes[0].roots.push_back(std::move(a));
    p.scenes[0].roots.push_back(std::move(a2));

    Node b = make_node(NodeKind::Precomp, "b");
    std::get<PrecompData>(b.data).scene = p.scenes[2].id;
    p.scenes[1].roots.push_back(std::move(b));

    CHECK_MSG(is_valid(p), "{}", errors_of(p));
}

LTEST_CASE(a_bus_that_sends_to_itself_is_an_error) {
    Project p = make_default_project("Show");
    p.buses[0].sends.push_back(Send{p.buses[0].id, 0.0, false});
    CHECK(!is_valid(p));
    CHECK(errors_of(p).find("sends to itself") != std::string::npos);
}

LTEST_CASE(an_enabled_output_needs_somewhere_to_go) {
    Project p = make_default_project("Show");
    Encode e;
    e.id = Id::generate();
    e.name = "1080p60 6 Mbps";
    e.canvas = p.canvases[0].id;
    p.encodes.push_back(e);

    Output o;
    o.id = Id::generate();
    o.name = "YouTube";
    o.encode = e.id;
    o.enabled = true;
    p.outputs.push_back(o);
    CHECK(!is_valid(p));

    p.outputs[0].destination.url = "rtmps://a.rtmps.youtube.com/live2";
    p.outputs[0].destination.credential_ref = "youtube-main";
    CHECK_MSG(is_valid(p), "{}", errors_of(p));

    // An output whose bitstream does not exist has nothing to send: an error, like a deck with
    // no canvas, rather than a dangling reference (ADR-0029).
    p.outputs[0].encode = Id::generate();
    CHECK(!is_valid(p));
    p.outputs[0].encode = e.id;

    // And an encode of a canvas that does not exist is caught in the same way.
    p.encodes[0].canvas = Id::generate();
    CHECK(!is_valid(p));
}

LTEST_CASE(a_newer_schema_version_is_refused_rather_than_partly_read) {
    Project p = make_default_project("Show");
    p.schema_version = kSchemaVersion + 1;
    CHECK(!is_valid(p));
    CHECK(errors_of(p).find("newer than this build") != std::string::npos);
}

LTEST_CASE(scene_audio_routing_for_a_node_that_is_not_there_is_a_warning_not_an_error) {
    // Harmless but useless: it applies to nothing. Worth saying, not worth refusing to open.
    Project p = make_default_project("Show");
    AudioOverride ov;
    ov.mute = true;
    p.scenes[0].audio_overrides.emplace(Id::generate(), ov);
    CHECK_MSG(is_valid(p), "{}", errors_of(p));
    CHECK(!validate(p).empty());
    CHECK(validate(p).front().severity == Severity::Warning);
}

// ---- the emergency scene (ADR-0025) ---------------------------------------------------------

namespace {

/// A project whose canvas has a proper safety net: one scene, one source, one looping local clip.
struct WithEmergencyScene {
    Project project;
    Id canvas;
    Id scene;
    Id source;
    Id node;

    explicit WithEmergencyScene(std::string type = "source.media", bool loops = true) {
        project = make_default_project("Show");
        canvas = project.canvases[0].id;

        Source src;
        src.id = Id::generate();
        src.name = "Be Right Back";
        src.type = std::move(type);
        src.settings.emplace("path", std::string{"assets/brb.webm"});
        src.settings.emplace("loop", loops);
        source = src.id;
        project.sources.push_back(std::move(src));

        Scene s;
        s.id = Id::generate();
        s.name = "Emergency";
        s.canvas = canvas;
        Node l = source_node("Card");
        std::get<SourceData>(l.data).source = source;
        node = l.id;
        s.roots.push_back(std::move(l));
        scene = s.id;
        project.scenes.push_back(std::move(s));

        project.canvases[0].emergency_scene = scene;
    }
};

} // namespace

LTEST_CASE(source_types_are_classified_by_whether_they_can_fault) {
    for (std::string_view safe : {"source.media", "source.image", "source.color", "source.text"})
        CHECK_MSG(is_self_contained(safe), "{} should be safe for an emergency scene", safe);

    // Anything with a device, a network, or a browser engine behind it can be exactly what
    // failed.
    for (std::string_view unsafe : {"source.browser", "source.v4l2", "source.screen",
                                    "source.window", "source.stream", "source.audio-input"})
        CHECK_MSG(!is_self_contained(unsafe), "{} should not be safe", unsafe);

    // And an unrecognised type answers false, because a net we cannot vouch for is not a net.
    CHECK(!is_self_contained("source.something-added-later"));
    CHECK(!is_self_contained(""));
}

LTEST_CASE(a_single_looping_local_clip_is_a_good_emergency_scene) {
    const WithEmergencyScene f;
    const auto problems = check_emergency_scene(f.project, f.scene);
    CHECK_MSG(problems.empty(), "{}", problems.empty() ? std::string{} : problems.front().message);
    CHECK_MSG(is_valid(f.project), "{}", errors_of(f.project));
    CHECK_EQ(warning_count(f.project), std::size_t{0});
}

LTEST_CASE(a_clip_that_does_not_loop_is_a_net_that_expires) {
    // The safe state can last minutes. A clip that plays once and cuts to black is worse than
    // useless, because it looks like it worked for the first thirty seconds.
    const WithEmergencyScene f{"source.media", /*loops=*/false};
    const auto problems = check_emergency_scene(f.project, f.scene);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().message.find("loop") != std::string::npos);
    CHECK(problems.front().severity == Severity::Warning);
    CHECK_MSG(is_valid(f.project), "an unsuitable emergency scene made the project invalid");
}

LTEST_CASE(a_source_that_can_fault_is_refused_as_a_safety_net) {
    for (const std::string_view type : {"source.browser", "source.v4l2", "source.screen"}) {
        const WithEmergencyScene f{std::string{type}};
        const auto problems = check_emergency_scene(f.project, f.scene);
        REQUIRE_MSG(!problems.empty(), "{} was accepted as a safety net", type);
        CHECK_MSG(problems.front().message.find(type) != std::string::npos,
                  "the complaint does not name the source type");
        CHECK(problems.front().message.find("Be Right Back") != std::string::npos);
    }
}

LTEST_CASE(an_empty_or_crowded_emergency_scene_is_reported) {
    Project p = make_default_project("Show");
    Scene empty;
    empty.id = Id::generate();
    empty.name = "Nothing";
    empty.canvas = p.canvases[0].id;
    p.scenes.push_back(empty);
    p.canvases[0].emergency_scene = empty.id;
    REQUIRE(check_emergency_scene(p, empty.id).size() == 1);
    CHECK(check_emergency_scene(p, empty.id).front().message.find("nothing in it") !=
          std::string::npos);

    // More sources is not an error -- a logo over a clip is reasonable -- but the net is reliable
    // in proportion to how little it depends on, and one pre-rendered clip can carry branding.
    WithEmergencyScene f;
    Scene* s = f.project.scene(f.scene);
    REQUIRE(s != nullptr);
    s->roots.push_back(source_node("Extra"));
    const auto problems = check_emergency_scene(f.project, f.scene);
    // Two complaints: the extra source node is bound to nothing, and the scene is no longer one clip.
    REQUIRE(problems.size() == 2);
    CHECK(problems.back().message.find("sources") != std::string::npos);
}

LTEST_CASE(a_source_node_bound_to_nothing_is_not_a_safety_net) {
    // It draws nothing, which makes the net exactly as useful as not having one -- while looking
    // in the scene list as though it is set, which is worse.
    Project p = make_default_project("Show");
    Scene s;
    s.id = Id::generate();
    s.name = "Emergency";
    s.canvas = p.canvases[0].id;
    s.roots.push_back(source_node("Card"));
    p.canvases[0].emergency_scene = s.id;
    p.scenes.push_back(std::move(s));

    const auto problems = check_emergency_scene(p, *p.canvases[0].emergency_scene);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().message.find("not bound to anything") != std::string::npos);
}

LTEST_CASE(an_emergency_scene_that_was_deleted_is_noticed) {
    WithEmergencyScene f;
    const auto it = std::find_if(f.project.scenes.begin(), f.project.scenes.end(),
                                 [&f](const Scene& s) { return s.id == f.scene; });
    f.project.scenes.erase(it);

    const auto problems = check_emergency_scene(f.project, f.scene);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().message.find("does not exist") != std::string::npos);
    CHECK_MSG(is_valid(f.project), "losing the safety net made the project unopenable");
    CHECK_EQ(warning_count(f.project), std::size_t{1});
}

LTEST_CASE(a_canvas_with_no_emergency_scene_is_not_a_document_problem) {
    // A project at rest is not wrong for lacking a safety net; warning about it would mean every
    // new project is born complaining. "Does every canvas have one?" is a pre-flight question,
    // asked when the operator is about to go live and it actually matters.
    const Project p = make_default_project("Show");
    CHECK(!p.canvases[0].emergency_scene.has_value());
    CHECK_MSG(validate(p).empty(), "a new project reported a problem: {}",
              validate(p).empty() ? std::string{} : validate(p).front().message);
}

LTEST_CASE(the_emergency_scene_is_per_canvas) {
    // A vertical show wants a separately authored vertical clip, and a per-canvas override can
    // reposition a card but cannot swap the source binding.
    WithEmergencyScene f;
    Canvas vertical;
    vertical.id = Id::generate();
    vertical.name = "Vertical";
    vertical.width = 1080;
    vertical.height = 1920;
    f.project.canvases.push_back(vertical);

    CHECK(f.project.canvases[0].emergency_scene.value() == f.scene);
    CHECK(!f.project.canvases[1].emergency_scene.has_value());
    CHECK_MSG(is_valid(f.project), "{}", errors_of(f.project));
}

LTEST_CASE(ignored_source_audio_preserves_routes_without_missing_bus_warnings) {
    for (const auto* type : {"source.media", "source.playlist", "source.browser"}) {
        auto project = make_default_project("Video only");
        Source source;
        source.id = Id::generate();
        source.type = type;
        CHECK(has_audio(source));
        auto node = make_node(NodeKind::Source, "Video");
        auto& audio = std::get<SourceData>(node.data).audio;
        std::get<SourceData>(node.data).source = source.id;
        audio.bus = Id::generate();
        audio.sends = {{Id::generate(), 0, false}};
        Scene scene;
        scene.id = Id::generate();
        scene.canvas = project.canvases.front().id;
        scene.roots = {node};
        scene.audio_overrides[node.id].bus = Id::generate();
        project.scenes = {scene};
        project.sources = {source};
        const auto bus_warnings = [&] {
            const auto problems = validate(project);
            return std::ranges::count_if(problems, [&](const Problem& problem) {
                return problem.subject == node.id && problem.message.contains("refers to bus");
            });
        };
        CHECK_EQ(bus_warnings(), 2);
        project.sources.front().settings["ignoreAudio"] = true;
        CHECK(!has_audio(project.sources.front()));
        CHECK_EQ(bus_warnings(), 0);
        const auto ignored_problems = validate(project);
        CHECK(std::ranges::none_of(ignored_problems, [](const Problem& problem) {
            return problem.message.contains("routes a source to bus");
        }));
        CHECK(project.scenes.front().audio_overrides.at(node.id).bus ==
              scene.audio_overrides.at(node.id).bus);
        CHECK(std::get<SourceData>(project.scenes.front().roots.front().data).audio == audio);
        project.sources.front().settings["ignoreAudio"] = false;
        CHECK(has_audio(project.sources.front()));
        CHECK_EQ(bus_warnings(), 2);
        project.sources.front().settings["ignoreAudio"] = std::string{"true"};
        CHECK(has_audio(project.sources.front()));
    }
}

LTEST_MAIN("document")
