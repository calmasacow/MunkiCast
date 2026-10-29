// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command bus: every mutation validated, reversible and serialisable.

#include "ldoc/command.hpp"
#include "ldoc/history.hpp"
#include "ldoc/json.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ldoc;

namespace {

/// A project with a couple of scenes, a group with children, and an effect to poke at.
struct Fixture {
    Project project;
    Id scene;
    Id group;
    Id child_a;
    Id child_b;
    Id loose;
    Id effect;
    Id canvas;
    Id deck;
    Id existing_source;
    Id spare_canvas;
    Id existing_bus;
    Id existing_encode;
    Id unused_encode;
    Id encoded_bus;
    Id existing_audio_encode;
    Id unused_audio_encode;
    Id existing_output;
    Id spare_scene;
    Id vertical_scene;
    Id existing_link;

    Fixture() {
        project = make_default_project("Show");
        canvas = project.canvases[0].id;
        deck = project.decks[0].id;
        scene = project.scenes[0].id;

        Node g = make_node(NodeKind::Group, "Group");
        group = g.id;
        Node a = make_node(NodeKind::Source, "Camera");
        child_a = a.id;
        Node b = make_node(NodeKind::Source, "Overlay");
        child_b = b.id;
        g.children()->push_back(std::move(a));
        g.children()->push_back(std::move(b));

        Node l = make_node(NodeKind::Source, "Background");
        loose = l.id;
        Effect e;
        e.id = Id::generate();
        e.type = "effect.blur";
        e.params.emplace("radius", 4.0);
        effect = e.id;
        l.effects.push_back(std::move(e));

        project.scenes[0].roots.push_back(std::move(g));
        project.scenes[0].roots.push_back(std::move(l));

        Source src;
        src.id = Id::generate();
        src.name = "Desk Cam";
        src.type = "source.v4l2";
        existing_source = src.id;
        project.sources.push_back(std::move(src));

        // A second scene, so reordering is a real move rather than a clamp to where it already
        // is -- which check_reversible would rightly call "applying it changed nothing".
        Scene second;
        second.id = Id::generate();
        second.name = "Second";
        second.canvas = canvas;
        spare_scene = second.id;
        project.scenes.push_back(std::move(second));

        existing_bus = project.buses.front().id;

        Encode enc;
        enc.id = Id::generate();
        enc.name = "1080p60 9 Mbps";
        enc.canvas = canvas;
        enc.encoder.bitrate_kbps = 9000;
        existing_encode = enc.id;
        project.encodes.push_back(std::move(enc));

        // An encode nothing sends, so the removal case has one it is allowed to take.
        Encode unused;
        unused.id = Id::generate();
        unused.name = "720p30 2 Mbps";
        unused.canvas = canvas;
        unused.encoder.bitrate_kbps = 2000;
        unused_encode = unused.id;
        project.encodes.push_back(std::move(unused));

        // A bus of their own: `existing_bus` is the one the removal cases take, and an audio
        // encode reading it would make every one of those a refusal.
        Bus encoded;
        encoded.id = Id::generate();
        encoded.name = "Stream mix";
        encoded.kind = BusKind::Program;
        encoded_bus = encoded.id;
        project.buses.push_back(std::move(encoded));

        AudioEncode aac;
        aac.id = Id::generate();
        aac.name = "AAC 160";
        aac.bus = encoded_bus;
        aac.bitrate_kbps = 160;
        existing_audio_encode = aac.id;
        project.audio_encodes.push_back(std::move(aac));

        // One nothing sends, so the removal case has one it is allowed to take.
        AudioEncode spare_aac;
        spare_aac.id = Id::generate();
        spare_aac.name = "AAC 320";
        spare_aac.bus = encoded_bus;
        spare_aac.bitrate_kbps = 320;
        unused_audio_encode = spare_aac.id;
        project.audio_encodes.push_back(std::move(spare_aac));

        Output out;
        out.id = Id::generate();
        out.name = "YouTube";
        out.encode = existing_encode;
        out.destination.url = "rtmps://example/live";
        out.destination.credential_ref = "youtube-main";
        existing_output = out.id;
        project.outputs.push_back(std::move(out));

        // A canvas nothing renders to, so the removal case has something it is allowed to take.
        Canvas spare;
        spare.id = Id::generate();
        spare.name = "Spare";
        spare_canvas = spare.id;
        project.canvases.push_back(std::move(spare));

        // A canvas needs a scene of its own before anything can be cued to it, and a link needs
        // two scenes on two canvases. Note this lives on `spare_canvas`, so removing that canvas
        // is now refused for a scene rather than for a deck -- which is the newer rule and the
        // one worth exercising.
        Canvas vertical_canvas;
        vertical_canvas.id = Id::generate();
        vertical_canvas.name = "Vertical";
        vertical_canvas.width = 1080;
        vertical_canvas.height = 1920;
        project.canvases.push_back(vertical_canvas);

        Scene vertical;
        vertical.id = Id::generate();
        vertical.name = "Vertical";
        vertical.canvas = vertical_canvas.id;
        vertical_scene = vertical.id;
        project.scenes.push_back(std::move(vertical));

        SceneLink link;
        link.id = Id::generate();
        link.from = scene;
        link.to = vertical_scene;
        existing_link = link.id;
        project.scene_links.push_back(link);
    }
};

/// Apply, check it took, apply the inverse, check the project is exactly as it started.
///
/// The command is built from a fixture made *here*, because every Fixture mints fresh ULIDs:
/// a command carrying ids from a different one refers to nothing.
void check_reversible(const char* what,
                      const std::function<Command(const Fixture&)>& make_command) {
    Fixture f;
    const Project before = f.project;
    const Command command = make_command(f);

    auto applied = apply(f.project, command);
    REQUIRE_MSG(applied.has_value(), "{}: {}", what, applied.error());
    CHECK_MSG(!(f.project == before), "{}: applying it changed nothing", what);
    CHECK_MSG(!applied->description.empty(), "{}: has no description", what);

    auto undone = apply(f.project, applied->inverse);
    REQUIRE_MSG(undone.has_value(), "{} (inverse): {}", what, undone.error());
    CHECK_MSG(f.project == before, "{}: undoing it did not restore the project exactly", what);

    // And the inverse of the inverse puts it back, which is what redo relies on.
    auto redone = apply(f.project, undone->inverse);
    REQUIRE_MSG(redone.has_value(), "{} (redo): {}", what, redone.error());
}

std::vector<Command> every_command_shape(const Fixture& f) {
    Deck addedDeck;
    addedDeck.id = Id::generate();
    addedDeck.canvas = f.spare_canvas;
    addedDeck.name = "Secondary";
    Scene fresh;
    fresh.id = Id::generate();
    fresh.name = "Added";
    fresh.canvas = f.canvas;

    Effect e;
    e.id = Id::generate();
    e.type = "effect.stroke";
    e.params.emplace("width", 2.0);

    ParamMap params;
    params.emplace("radius", 9.0);

    Transform t;
    t.position = Vec2{10.0, 20.0};
    Layout lay;
    lay.fit = FitMode::Cover;
    Blend bl;
    bl.mode = BlendMode::Multiply;
    bl.opacity = 0.5;

    Source src;
    src.id = Id::generate();
    src.name = "Camera";
    src.type = "source.v4l2";
    src.settings.emplace("device", std::string{"/dev/video0"});

    Source changed = f.project.sources.empty() ? src : f.project.sources.front();
    changed.name = "Renamed source";

    Canvas vertical;
    vertical.id = Id::generate();
    vertical.name = "Vertical";
    vertical.width = 1080;
    vertical.height = 1920;

    Canvas resized = f.project.canvases.front();
    resized.width = 2560;
    resized.height = 1440;

    Deck studio = f.project.decks.front();
    studio.studio_mode = true;
    studio.transition = "transition.fade";
    studio.transition_ms = 500;

    AudioRouting routed;
    routed.bus = f.project.buses.front().id;
    routed.gain_db = -6.0;
    routed.sends.push_back(Send{f.project.buses.back().id, -3.0, true});

    Bus submix;
    submix.id = Id::generate();
    submix.name = "Game";
    submix.kind = BusKind::Submix;

    Bus louder = f.project.buses.front();
    louder.gain_db = -3.0;

    Output twitch;
    twitch.id = Id::generate();
    twitch.name = "Twitch";
    twitch.encode = f.existing_encode;
    twitch.destination.url = "rtmp://ingest.example/app";

    Output renamed_output = f.project.outputs.front();
    renamed_output.name = "YouTube Main";

    Encode spare_encode;
    spare_encode.id = Id::generate();
    spare_encode.name = "720p60 3 Mbps";
    spare_encode.canvas = f.canvas;
    spare_encode.encoder.bitrate_kbps = 3000;

    Encode rebitrated = f.project.encodes.front();
    rebitrated.encoder.bitrate_kbps = 12000;

    AudioEncode spare_audio_encode;
    spare_audio_encode.id = Id::generate();
    spare_audio_encode.name = "Opus 128";
    spare_audio_encode.bus = f.encoded_bus;
    spare_audio_encode.codec = "opus";
    spare_audio_encode.bitrate_kbps = 128;

    AudioEncode rebitrated_audio = f.project.audio_encodes.front();
    rebitrated_audio.bitrate_kbps = 256;

    Settings tuned = f.project.settings;
    tuned.sample_rate = 44100;

    return {
        SetProjectName{"Renamed"},
        AddScene{fresh, 1},
        RemoveScene{f.scene},
        RenameScene{f.scene, "Intro"},
        MoveScene{f.scene, 1},
        AddNode{f.scene, f.group, 1, make_node(NodeKind::Source, "New")},
        RemoveNode{f.scene, f.child_a},
        MoveNode{f.scene, f.child_a, std::nullopt, 0},
        RenameNode{f.scene, f.loose, "Renamed"},
        SetNodeVisible{f.scene, f.loose, false},
        SetNodeLocked{f.scene, f.loose, true},
        SetNodeTransform{f.scene, f.loose, t},
        SetNodeLayout{f.scene, f.loose, lay},
        SetNodeBlend{f.scene, f.loose, bl},
        AddEffect{f.scene, f.loose, 0, e},
        SetEffects{f.existing_source, {e}},
        RemoveEffect{f.scene, f.loose, f.effect},
        SetEffectParams{f.scene, f.loose, f.effect, params},
        SetEffectEnabled{f.scene, f.loose, f.effect, false},
        SetDeckProgram{f.deck, std::nullopt},
        SetDeckPreview{f.deck, f.scene},
        AddSceneLink{SceneLink{Id::generate(), f.spare_scene, f.vertical_scene}, 0},
        RemoveSceneLink{f.existing_link},
        SetCanvasEmergencyScene{f.canvas, f.scene},
        SetNodeSource{f.scene, f.loose, f.existing_source},
        AddSource{src, 0},
        RemoveSource{f.existing_source},
        SetSource{changed},
        AddCanvas{vertical, 1},
        RemoveCanvas{f.spare_canvas},
        SetCanvas{resized},
        SetSceneFavourite{f.scene, true},
        make_batch({RenameScene{f.scene, "Batched"}, SetSceneFavourite{f.scene, true}},
                   "Rename and star"),
        SetNodeAudio{f.scene, f.loose, routed},
        SetDeck{studio},
        AddDeck{addedDeck, f.project.decks.size()},
        RemoveDeck{f.deck},
        AddBus{submix, 0},
        RemoveBus{f.existing_bus},
        SetBus{louder},
        AddOutput{twitch, 0},
        RemoveOutput{f.existing_output},
        SetOutput{renamed_output},
        AddEncode{spare_encode, 0},
        RemoveEncode{f.unused_encode},
        SetEncode{rebitrated},
        AddAudioEncode{spare_audio_encode, 0},
        RemoveAudioEncode{f.unused_audio_encode},
        SetAudioEncode{rebitrated_audio},
        SetProjectSettings{tuned},
    };
}

/// A random command that is plausible against the given project. Nothing here guarantees it
/// applies -- half the value is in the ones that get refused.
Command random_command(std::mt19937& rng, const Project& p) {
    // The sequence can legitimately delete every scene, so there is not always one to pick.
    if (p.scenes.empty() || p.canvases.empty() || p.decks.empty()) {
        Scene fresh;
        fresh.id = Id::generate();
        fresh.name = std::format("Scene {}", rng() % 100);
        // Main can never be removed, so there is always a canvas to put it on.
        if (!p.canvases.empty())
            fresh.canvas = p.canvases.front().id;
        return AddScene{fresh, 0};
    }
    const Scene& scene = p.scenes[rng() % p.scenes.size()];
    std::vector<Id> nodes;
    collect_defined_ids(scene.roots, nodes);
    const Id node = nodes.empty() ? Id::generate() : nodes[rng() % nodes.size()];
    const Id canvas = p.canvases[rng() % p.canvases.size()].id;
    const Id deck = p.decks[rng() % p.decks.size()].id;
    // A sequence can remove every bus, which the project is allowed to be left with -- so
    // unlike a canvas or a deck there is not always one to route at.
    const Id bus = p.buses.empty() ? Id::generate() : p.buses[rng() % p.buses.size()].id;

    switch (rng() % 29) {
    case 0:
        return SetProjectName{std::format("Show {}", rng() % 100)};
    case 1: {
        Scene fresh;
        fresh.id = Id::generate();
        fresh.name = std::format("Scene {}", rng() % 100);
        fresh.canvas = canvas;
        if (rng() % 2)
            fresh.roots.push_back(make_node(NodeKind::Source, "l"));
        return AddScene{fresh, rng() % (p.scenes.size() + 1)};
    }
    case 2:
        return RemoveScene{scene.id};
    case 3:
        return RenameScene{scene.id, std::format("Scene {}", rng() % 100)};
    case 16:
        return MoveScene{scene.id, rng() % (p.scenes.size() + 2)};
    case 4: {
        const NodeKind kind = (rng() % 4 == 0) ? NodeKind::Group : NodeKind::Source;
        return AddNode{scene.id, (rng() % 2) ? std::optional<Id>{node} : std::nullopt,
                       rng() % 3, make_node(kind, std::format("n{}", rng() % 100))};
    }
    case 5:
        return RemoveNode{scene.id, node};
    case 6:
        return MoveNode{scene.id, node, (rng() % 2) ? std::optional<Id>{node} : std::nullopt,
                        rng() % 3};
    case 7:
        return RenameNode{scene.id, node, std::format("n{}", rng() % 100)};
    case 8:
        return SetNodeVisible{scene.id, node, (rng() % 2) == 0};
    case 9:
        return SetNodeLocked{scene.id, node, (rng() % 2) == 0};
    case 10: {
        Transform t;
        t.position = Vec2{static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000)};
        t.rotation_deg = static_cast<double>(rng() % 360);
        return SetNodeTransform{scene.id, node, t};
    }
    case 11: {
        Blend b;
        b.mode = static_cast<BlendMode>(rng() % 25);
        b.opacity = static_cast<double>(rng() % 101) / 100.0;
        return SetNodeBlend{scene.id, node, b};
    }
    case 12: {
        Layout lay;
        lay.fit = static_cast<FitMode>(rng() % 4);
        return SetNodeLayout{scene.id, node, lay};
    }
    case 13: {
        Effect e;
        e.id = Id::generate();
        e.type = "effect.blur";
        e.params.emplace("radius", static_cast<double>(rng() % 40));
        return AddEffect{scene.id, node, rng() % 2, e};
    }
    case 14:
        return SetDeckProgram{deck, (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    case 15:
        return SetCanvasEmergencyScene{canvas,
                                       (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    case 26:
        return SetSceneFavourite{scene.id, (rng() % 2) == 0};
    case 27: {
        // A batch of two ordinary commands. Built from the same generator so the members are as
        // varied as everything else, and capped at two so a refused member stays common -- the
        // unwind is the part worth hammering.
        std::vector<Command> members;
        for (int i = 0; i < 2; ++i) {
            Command member = random_command(rng, p);
            if (std::holds_alternative<Batch>(member))
                member = RenameScene{scene.id, std::format("s{}", rng() % 100)};
            members.push_back(std::move(member));
        }
        return make_batch(std::move(members), "A batch");
    }
    case 25: {
        Deck d = p.decks[rng() % p.decks.size()];
        d.studio_mode = (rng() % 2) == 0;
        d.transition_ms = static_cast<std::int32_t>(rng() % 1000);
        d.canvas = canvas;
        return SetDeck{d};
    }
    case 24: {
        AudioRouting a;
        if (rng() % 4)
            a.bus = bus;
        a.gain_db = static_cast<double>(rng() % 24) - 12.0;
        a.mute = (rng() % 2) == 0;
        return SetNodeAudio{scene.id, node, a};
    }
    case 17: {
        Bus b;
        b.id = Id::generate();
        b.name = std::format("Bus {}", rng() % 100);
        b.kind = static_cast<BusKind>(rng() % 3);
        b.gain_db = static_cast<double>(rng() % 24) - 12.0;
        return AddBus{b, rng() % (p.buses.size() + 1)};
    }
    case 18:
        return RemoveBus{bus};
    case 19: {
        if (p.buses.empty())
            return SetProjectName{"no buses"};
        Bus b = p.buses[rng() % p.buses.size()];
        b.gain_db = static_cast<double>(rng() % 24) - 12.0;
        b.mute = (rng() % 2) == 0;
        return SetBus{b};
    }
    case 20: {
        if (p.encodes.empty()) {
            Encode e;
            e.id = Id::generate();
            e.name = std::format("Encode {}", rng() % 100);
            e.canvas = canvas;
            e.encoder.bitrate_kbps = 1000 + static_cast<int>(rng() % 20000);
            return AddEncode{e, 0};
        }
        Output o;
        o.id = Id::generate();
        o.name = std::format("Out {}", rng() % 100);
        o.encode = p.encodes[rng() % p.encodes.size()].id;
        o.destination.url = "rtmp://example/app";
        return AddOutput{o, rng() % (p.outputs.size() + 1)};
    }
    case 21:
        return RemoveOutput{p.outputs.empty() ? Id::generate()
                                              : p.outputs[rng() % p.outputs.size()].id};
    case 22: {
        if (p.outputs.empty() || p.encodes.empty())
            return SetProjectName{"no outputs"};
        Output o = p.outputs[rng() % p.outputs.size()];
        o.name = std::format("Out {}", rng() % 100);
        o.encode = p.encodes[rng() % p.encodes.size()].id;
        return SetOutput{o};
    }
    case 23: {
        Settings st = p.settings;
        st.sample_rate = (rng() % 2) ? 48000 : 44100;
        return SetProjectSettings{st};
    }
    default:
        return SetDeckPreview{deck, (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    }
}

} // namespace

LTEST_CASE(every_command_applies_and_reverses_exactly) {
    const Fixture f;
    const auto commands = every_command_shape(f);
    CHECK_MSG(commands.size() == std::variant_size_v<Command>,
              "{} of {} command types are covered", commands.size(),
              std::variant_size_v<Command>);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const std::string name{command_name(commands[i])};
        check_reversible(name.c_str(),
                         [i](const Fixture& fx) { return every_command_shape(fx)[i]; });
    }
}

LTEST_CASE(every_command_has_a_distinct_name_and_a_description) {
    const Fixture f;
    std::set<std::string> names;
    for (const Command& c : every_command_shape(f)) {
        const std::string name{command_name(c)};
        CHECK_MSG(names.insert(name).second, "'{}' is used by two commands", name);
        CHECK(!name.empty());
        CHECK_MSG(!describe(c).empty(), "{} has no description", name);
    }
    CHECK_EQ(names.size(), std::variant_size_v<Command>);
}

LTEST_CASE(a_refused_command_changes_nothing) {
    Fixture f;
    const Id nowhere = Id::generate();

    // Each of these is missing the thing it would act on, which is a precondition rather than a
    // reference, so each is refused.
    const std::vector<Command> bad = {
        RenameScene{nowhere, "x"},
        RemoveScene{nowhere},
        AddNode{nowhere, std::nullopt, 0, make_node(NodeKind::Source, "n")},
        RemoveNode{f.scene, nowhere},
        RenameNode{f.scene, nowhere, "x"},
        SetNodeVisible{f.scene, nowhere, false},
        SetNodeLocked{f.scene, nowhere, true},
        SetEffectParams{f.scene, f.loose, nowhere, {}},
        SetEffectEnabled{f.scene, f.loose, nowhere, false},
        RemoveEffect{f.scene, f.loose, nowhere},
        SetDeckProgram{nowhere, std::nullopt},
        SetDeckPreview{nowhere, std::nullopt},
        AddDeck{Deck{}, 0},
        AddDeck{f.project.decks.front(), 0},
        RemoveDeck{nowhere},
        AddSceneLink{SceneLink{Id::generate(), f.scene, nowhere}, 0},
        AddSceneLink{SceneLink{Id::generate(), f.scene, f.spare_scene}, 0},
        AddSceneLink{SceneLink{Id::generate(), f.scene, f.vertical_scene}, 0},
        RemoveSceneLink{nowhere},
        AddScene{Scene{}, 99},
        AddNode{f.scene, std::nullopt, 99, make_node(NodeKind::Source, "n")},
        AddEffect{f.scene, nowhere, 0, Effect{}},
    };
    for (const Command& c : bad) {
        const Project before = f.project;
        const auto r = apply(f.project, c);
        CHECK_MSG(!r.has_value(), "{} was accepted when it should not have been",
                  command_name(c));
        CHECK_MSG(f.project == before, "{} mutated the project before failing",
                  command_name(c));
    }
}

LTEST_CASE(pointing_at_something_that_does_not_exist_is_allowed_and_reported_later) {
    // The other side of the precondition/reference line. These produce a document the validator
    // complains about, and that is the right place for the complaint: refusing them here would
    // make undo unable to restore a state the model permits, and would mean nothing could be
    // deleted while anything still named it.
    Fixture f;
    const Id nowhere = Id::generate();

    REQUIRE(apply(f.project, SetDeckProgram{f.deck, nowhere}).has_value());
    CHECK_MSG(is_valid(f.project), "a dangling deck pointer was treated as unrenderable");
    CHECK(!validate(f.project).empty());

    REQUIRE(apply(f.project, SetNodeSource{f.scene, f.loose, nowhere}).has_value());
    CHECK(is_valid(f.project));
    const auto problems = validate(f.project);
    CHECK(std::all_of(problems.begin(), problems.end(), [](const Problem& p) {
        return p.severity == Severity::Warning;
    }));
    CHECK(problems.size() >= 2);
}

LTEST_CASE(a_node_cannot_be_added_with_an_id_the_project_already_uses) {
    // Two nodes with one id is a corruption the validator would catch later; the command should
    // never be able to create it.
    Fixture f;
    const Project before = f.project;
    Node clash = make_node(NodeKind::Source, "clash");
    clash.id = f.child_a;
    const auto r = apply(f.project, AddNode{f.scene, std::nullopt, 0, clash});
    CHECK(!r.has_value());
    CHECK(r.error().find("already in the project") != std::string::npos);
    CHECK(f.project == before);

    // Including an id buried inside the subtree being added.
    Node g = make_node(NodeKind::Group, "g");
    Node inner = make_node(NodeKind::Source, "inner");
    inner.id = f.loose;
    g.children()->push_back(std::move(inner));
    CHECK(!apply(f.project, AddNode{f.scene, std::nullopt, 0, g}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(only_a_group_can_be_added_to) {
    Fixture f;
    const Project before = f.project;
    const auto r = apply(f.project, AddNode{f.scene, f.loose, 0,
                                            make_node(NodeKind::Source, "n")});
    CHECK(!r.has_value());
    CHECK(r.error().find("not a group") != std::string::npos);
    CHECK(f.project == before);
}

LTEST_CASE(a_group_cannot_be_moved_inside_itself) {
    // It would detach the whole subtree and leave the tree referring to nodes nothing owns.
    Fixture f;
    const Project before = f.project;

    const auto into_self = apply(f.project, MoveNode{f.scene, f.group, f.group, 0});
    CHECK(!into_self.has_value());
    CHECK(f.project == before);

    const auto into_child = apply(f.project, MoveNode{f.scene, f.group, f.child_a, 0});
    CHECK_MSG(!into_child.has_value(), "a group was moved inside its own child");
    CHECK(f.project == before);

    // Moving a child out to the root is fine, and reverses.
    check_reversible("move out", [](const Fixture& fx) {
        return Command{MoveNode{fx.scene, fx.child_a, std::nullopt, 0}};
    });
}

LTEST_CASE(removing_a_group_takes_its_children_and_undo_brings_them_all_back) {
    Fixture f;
    const Project before = f.project;
    auto applied = apply(f.project, RemoveNode{f.scene, f.group});
    REQUIRE(applied.has_value());

    std::vector<Id> left;
    collect_defined_ids(f.project.scenes[0].roots, left);
    CHECK(std::find(left.begin(), left.end(), f.child_a) == left.end());
    CHECK(std::find(left.begin(), left.end(), f.child_b) == left.end());

    REQUIRE(apply(f.project, applied->inverse).has_value());
    CHECK_MSG(f.project == before, "undoing a group removal did not restore its children");
}

LTEST_CASE(an_opacity_outside_the_legal_range_is_refused) {
    Fixture f;
    const Project before = f.project;
    Blend b;
    b.opacity = 1.5;
    CHECK(!apply(f.project, SetNodeBlend{f.scene, f.loose, b}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(every_command_survives_a_json_round_trip) {
    const Fixture f;
    for (const Command& c : every_command_shape(f)) {
        const std::string text = command_to_json(c);
        const auto back = command_from_json(text);
        REQUIRE_MSG(back.has_value(), "{}: {}", command_name(c), back.error());
        CHECK_MSG(*back == c, "{} changed on the way through JSON", command_name(c));
        CHECK_MSG(command_to_json(*back) == text, "{} re-serialised differently",
                  command_name(c));
        CHECK(text.find(std::string{command_name(c)}) != std::string::npos);
    }
}

LTEST_CASE(filter_stacks_are_shared_persistent_commands_with_exact_undo) {
    const Fixture f;
    for (const auto owner : {f.existing_source, f.scene, f.group, f.existing_bus}) {
        DocumentHistory history{f.project};
        Effect effect;
        effect.id = Id::generate();
        effect.type = "filter.color-correction";
        effect.name = "Warm camera";
        effect.enabled = false;
        effect.params.emplace("brightness", 0.25);
        const Command command = SetEffects{owner, {effect}};
        const auto decoded = command_from_json(command_to_json(command));
        REQUIRE(decoded);
        CHECK(*decoded == command);
        REQUIRE(history.run(*decoded));
        REQUIRE(history.project().effect_stack(owner));
        CHECK(*history.project().effect_stack(owner) == std::vector<Effect>{effect});
        REQUIRE(history.undo());
        CHECK(history.project() == f.project);
        REQUIRE(history.redo());
        CHECK(*history.project().effect_stack(owner) == std::vector<Effect>{effect});
        const auto before = history.project();
        CHECK(!history.run(SetEffects{owner, {effect, effect}}));
        effect.id = f.scene;
        CHECK(!history.run(SetEffects{owner, {effect}}));
        CHECK(history.project() == before);
    }
}

LTEST_CASE(a_command_this_build_does_not_know_is_refused) {
    // Skipping it would make a journal replay a different session than it recorded.
    const auto r = command_from_json(R"({"cmd": "node.teleport", "scene": "x"})");
    CHECK(!r.has_value());
    CHECK(r.error().find("node.teleport") != std::string::npos);

    for (const std::string& bad : {std::string{"{}"}, std::string{"not json"},
                                   std::string{R"({"cmd": 7})"},
                                   std::string{R"({"cmd": "scene.rename"})"},
                                   std::string{R"({"cmd": "scene.rename", "scene": "nope"})"}})
        CHECK_MSG(!command_from_json(bad).has_value(), "accepted {}", bad);
}

LTEST_CASE(history_undoes_and_redoes_in_order_with_descriptions) {
    Fixture f;
    DocumentHistory h{f.project};
    const Project start = h.project();

    REQUIRE(h.run(RenameNode{f.scene, f.loose, "First"}).has_value());
    REQUIRE(h.run(SetNodeVisible{f.scene, f.loose, false}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{2});
    // The description names the node as it was called *when the command ran*, which is after
    // the rename above.
    CHECK_EQ(h.undo_description(), std::string("Hide 'First'"));

    REQUIRE(h.undo().has_value());
    CHECK(find_node(h.project().scenes[0].roots, f.loose)->visible);
    CHECK_EQ(h.undo_description(), std::string("Rename source to 'First'"));
    CHECK_EQ(h.redo_description(), std::string("Hide 'First'"));

    REQUIRE(h.undo().has_value());
    CHECK_MSG(h.project() == start, "undoing everything did not restore the project");
    CHECK(!h.can_undo());
    CHECK(!h.undo().has_value());

    REQUIRE(h.redo().has_value());
    REQUIRE(h.redo().has_value());
    CHECK_EQ(find_node(h.project().scenes[0].roots, f.loose)->name, std::string("First"));
    CHECK(!find_node(h.project().scenes[0].roots, f.loose)->visible);
    CHECK(!h.redo().has_value());
}

LTEST_CASE(a_refused_command_does_not_land_on_the_undo_stack) {
    // Otherwise Ctrl+Z after a command that did nothing undoes the previous real one.
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(RenameNode{f.scene, f.loose, "Kept"}).has_value());
    CHECK(!h.run(RenameNode{f.scene, Id::generate(), "x"}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{1});
    CHECK_EQ(h.undo_description(), std::string("Rename source to 'Kept'"));
}

LTEST_CASE(a_new_command_discards_the_redo_tail) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetNodeVisible{f.scene, f.loose, false}).has_value());
    REQUIRE(h.undo().has_value());
    CHECK(h.can_redo());
    REQUIRE(h.run(RenameNode{f.scene, f.loose, "Branch"}).has_value());
    CHECK(!h.can_redo());
}

LTEST_CASE(the_undo_stack_is_bounded) {
    Fixture f;
    DocumentHistory h{f.project, 8};
    for (int i = 0; i < 40; ++i)
        REQUIRE(h.run(RenameNode{f.scene, f.loose, std::format("n{}", i)}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{8});
    for (int i = 0; i < 8; ++i)
        CHECK(h.undo().has_value());
    CHECK(!h.can_undo());
    // Whatever is left is still a coherent project.
    CHECK(is_valid(h.project()));
}

LTEST_CASE(undo_can_restore_a_reference_that_has_since_become_dangling) {
    // Found by the random sequences below, and subtle enough to pin down on its own.
    //
    // Deleting a scene the deck points at is allowed (the deck then shows nothing, which the
    // validator reports as a warning). Pointing the deck somewhere else captures the *old*
    // pointer as its inverse -- and that pointer is now dangling. Undo still has to restore it,
    // because that is the state it is undoing. So a command may only refuse what the document
    // model itself refuses.
    Fixture f;
    DocumentHistory h{f.project};

    Scene spare;
    spare.id = Id::generate();
    spare.name = "Spare";
    spare.canvas = f.canvas;
    const Id spare_id = spare.id;
    REQUIRE(h.run(AddScene{spare, 1}).has_value());
    REQUIRE(h.run(SetDeckProgram{f.deck, spare_id}).has_value());

    // Delete the scene the deck is pointing at, then point the deck elsewhere.
    REQUIRE(h.run(RemoveScene{spare_id}).has_value());
    const Project dangling = h.project();
    CHECK(h.project().decks[0].program_scene.value() == spare_id);
    CHECK_MSG(is_valid(h.project()), "a dangling deck pointer was treated as an error");

    REQUIRE(h.run(SetDeckProgram{f.deck, f.scene}).has_value());
    const auto undone = h.undo();
    REQUIRE_MSG(undone.has_value(), "{}", undone ? std::string{} : undone.error());
    CHECK_MSG(h.project() == dangling, "undo did not restore the pointer it replaced");
}

LTEST_CASE(random_command_sequences_undo_and_redo_to_the_exact_same_project) {
    // The property the plan asks for by name. Random sequences, because the interesting failures
    // are in the orderings nobody writes a case for -- a remove whose undo has to restore a
    // subtree into the right parent at the right index, after other things have moved.
    std::mt19937 rng{20261012u};

    for (int iteration = 0; iteration < 120; ++iteration) {
        Fixture f;
        DocumentHistory h{f.project, 10000};
        const Project start = h.project();

        int applied = 0;
        int refused = 0;
        for (int step = 0; step < 60; ++step) {
            const Command c = random_command(rng, h.project());
            const Project before = h.project();
            if (h.run(c)) {
                ++applied;
            } else {
                ++refused;
                REQUIRE_MSG(h.project() == before,
                            "iteration {} step {}: a refused {} mutated the project", iteration,
                            step, command_name(c));
            }
            const auto bad = validate(h.project());
            const bool has_error = std::any_of(bad.begin(), bad.end(), [](const Problem& x) {
                return x.severity == Severity::Error;
            });
            REQUIRE_MSG(!has_error, "iteration {} step {}: {} left the project invalid: {}",
                        iteration, step, command_name(c),
                        bad.empty() ? std::string{} : bad.front().message);
        }
        REQUIRE_MSG(applied > 10, "iteration {}: only {} commands applied", iteration, applied);

        const Project finished = h.project();

        // Undo everything: the project must be byte-identical to where it started.
        const std::size_t depth = h.undo_depth();
        for (std::size_t i = 0; i < depth; ++i) {
            const std::string what = h.undo_description();
            const auto r = h.undo();
            REQUIRE_MSG(r.has_value(), "iteration {}: undo {} ('{}') failed: {}", iteration, i,
                        what, r ? std::string{} : r.error());
        }
        REQUIRE_MSG(h.project() == start,
                    "iteration {}: undoing {} command(s) did not restore the project",
                    iteration, depth);

        // Redo everything: and identical to where it ended.
        for (std::size_t i = 0; i < depth; ++i)
            REQUIRE_MSG(h.redo().has_value(), "iteration {}: redo {} failed", iteration, i);
        REQUIRE_MSG(h.project() == finished,
                    "iteration {}: redoing {} command(s) did not restore the project",
                    iteration, depth);
        (void)refused;
    }
}

LTEST_CASE(the_version_1_name_for_binding_a_source_is_still_read) {
    // The journal is replayed during crash recovery, which is the worst possible place for a
    // strict parser: refusing an entry written by last week's build loses exactly the work the
    // journal exists to save. So "layer.set-source" is still accepted (ADR-0028) -- and
    // "node.set-source" is the only name ever written.
    Fixture f;
    const Id source = f.existing_source;

    const auto old_name = command_from_json(
        std::format(R"({{"cmd":"layer.set-source","scene":"{}","node":"{}","source":"{}"}})",
                    f.scene.to_string(), f.loose.to_string(), source.to_string()));
    REQUIRE_MSG(old_name.has_value(), "{}", old_name.error());
    CHECK(std::holds_alternative<SetNodeSource>(*old_name));
    CHECK(apply(f.project, *old_name).has_value());

    CHECK_EQ(std::string{command_name(SetNodeSource{f.scene, f.loose, source})},
             std::string{"node.set-source"});
    CHECK_MSG(command_to_json(SetNodeSource{f.scene, f.loose, source}).find("layer.") ==
                  std::string::npos,
              "the old name was written back out");
}

LTEST_CASE(every_applied_command_survives_the_journal_shaped_round_trip) {
    // Each command is applied, serialised, parsed back and applied to a fresh copy; the two
    // projects must agree. This is replay, which is what the journal does after a crash.
    std::mt19937 rng{99u};
    Fixture f;
    Project live = f.project;
    Project replayed = f.project;

    int applied = 0;
    for (int step = 0; step < 400; ++step) {
        const Command c = random_command(rng, live);
        const auto direct = apply(live, c);
        if (!direct)
            continue;
        ++applied;

        const auto text = command_to_json(c);
        const auto parsed = command_from_json(text);
        REQUIRE_MSG(parsed.has_value(), "step {}: {}", step, parsed.error());
        const auto second = apply(replayed, *parsed);
        REQUIRE_MSG(second.has_value(), "step {}: replaying {} failed: {}", step,
                    command_name(c), second.error());
        REQUIRE_MSG(live == replayed, "step {}: replaying {} diverged", step, command_name(c));
    }
    CHECK(applied > 100);
}

LTEST_CASE(designating_an_emergency_scene_is_an_ordinary_undoable_command) {
    Fixture f;
    DocumentHistory h{f.project};
    CHECK(!h.project().canvases[0].emergency_scene.has_value());

    REQUIRE(h.run(SetCanvasEmergencyScene{f.canvas, f.scene}).has_value());
    CHECK(h.project().canvases[0].emergency_scene.value() == f.scene);
    CHECK_EQ(h.undo_description(), std::string("Set emergency scene"));

    REQUIRE(h.undo().has_value());
    CHECK_MSG(!h.project().canvases[0].emergency_scene.has_value(),
              "undoing the designation did not clear it");
    REQUIRE(h.redo().has_value());
    CHECK(h.project().canvases[0].emergency_scene.value() == f.scene);

    // Clearing it is the same command with nothing in it.
    REQUIRE(h.run(SetCanvasEmergencyScene{f.canvas, std::nullopt}).has_value());
    CHECK(!h.project().canvases[0].emergency_scene.has_value());
    CHECK_EQ(h.undo_description(), std::string("Clear emergency scene"));

    // The canvas is a precondition; the scene is not. ADR-0023: refusing a state the model
    // permits would make an inverse unapplicable and break undo at an arbitrary depth. Judging
    // whether a scene is a *suitable* safety net belongs to the designation flow, which runs
    // check_emergency_scene before it ever issues this.
    CHECK(!apply(f.project, SetCanvasEmergencyScene{Id::generate(), f.scene}).has_value());
    CHECK(apply(f.project, SetCanvasEmergencyScene{f.canvas, Id::generate()}).has_value());
}

LTEST_CASE(a_source_node_can_be_bound_to_a_source_and_unbound) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetNodeSource{f.scene, f.loose, f.existing_source}).has_value());
    const Node* n = find_node(h.project().scenes[0].roots, f.loose);
    REQUIRE(n != nullptr);
    CHECK(std::get<SourceData>(n->data).source.value() == f.existing_source);

    REQUIRE(h.undo().has_value());
    CHECK(!std::get<SourceData>(find_node(h.project().scenes[0].roots, f.loose)->data)
               .source.has_value());

    // A group is not a source node, so there is nothing to bind.
    CHECK(!apply(f.project, SetNodeSource{f.scene, f.group, f.existing_source}).has_value());
}

LTEST_CASE(a_canvas_that_something_renders_to_cannot_be_removed) {
    // Unlike a source, a scene, deck or output without a canvas has no output surface at all --
    // which validate() calls an error, so refusing here is the model's own rule rather than an
    // extra one. ADR-0023's line again: a command refuses exactly what the model refuses.
    Fixture f;
    const Project before = f.project;

    // Main is refused outright, before any of that (ADR-0027).
    const auto main = apply(f.project, RemoveCanvas{f.canvas});
    CHECK(!main.has_value());
    CHECK(main.error().find("Main cannot be removed") != std::string::npos);

    // The vertical one is not Main, but a scene belongs to it.
    const Id vertical = f.project.scene(f.vertical_scene)->canvas;
    const auto with_scene = apply(f.project, RemoveCanvas{vertical});
    CHECK(!with_scene.has_value());
    CHECK(with_scene.error().find("move or delete it first") != std::string::npos);

    // And a canvas held only by a deck names the deck.
    Deck second;
    second.id = Id::generate();
    second.name = "Spare deck";
    second.canvas = f.spare_canvas;
    f.project.decks.push_back(std::move(second));
    const auto with_deck = apply(f.project, RemoveCanvas{f.spare_canvas});
    CHECK(!with_deck.has_value());
    CHECK(with_deck.error().find("point it at another one first") != std::string::npos);

    f.project = before;
    CHECK(f.project == before);

    // The spare one, which nothing renders to and no scene lives on, comes away cleanly.
    check_reversible("remove spare canvas", [](const Fixture& fx) {
        return Command{RemoveCanvas{fx.spare_canvas}};
    });
}

LTEST_CASE(main_is_the_first_canvas_and_the_command_layer_says_so) {
    // The positional rule ("canvases[0] is Main") is only honest while nothing can take that
    // place, so the immutability is enforced here rather than hidden in the UI (ADR-0027).
    Fixture f;

    Canvas extra;
    extra.id = Id::generate();
    extra.name = "Before Main";
    const auto at_front = apply(f.project, AddCanvas{extra, 0});
    CHECK(!at_front.has_value());
    CHECK(at_front.error().find("nothing can be put before it") != std::string::npos);

    Canvas renamed = f.project.canvases[0];
    renamed.name = "Something Else";
    const auto rename = apply(f.project, SetCanvas{renamed});
    CHECK(!rename.has_value());
    CHECK(rename.error().find("Main cannot be renamed") != std::string::npos);

    // Everything else about Main is ordinary.
    Canvas resized = f.project.canvases[0];
    resized.width = 3840;
    resized.height = 2160;
    CHECK(apply(f.project, SetCanvas{resized}).has_value());
    CHECK_EQ(f.project.canvases[0].width, 3840);
}

LTEST_CASE(a_scene_link_obeys_the_three_rules_that_make_a_cue_unambiguous) {
    Fixture f;

    // Same canvas: a scene activating itself.
    const auto same = apply(f.project, AddSceneLink{
        SceneLink{Id::generate(), f.scene, f.spare_scene}, 0});
    CHECK(!same.has_value());
    CHECK(same.error().find("same canvas") != std::string::npos);

    // A second cue from the same scene to the same canvas: no answer to "which one".
    Scene another_vertical;
    another_vertical.id = Id::generate();
    another_vertical.name = "Vertical B";
    another_vertical.canvas = f.project.scene(f.vertical_scene)->canvas;
    REQUIRE(apply(f.project, AddScene{another_vertical, 0}).has_value());
    const auto twice = apply(f.project, AddSceneLink{
        SceneLink{Id::generate(), f.scene, another_vertical.id}, 0});
    CHECK(!twice.has_value());
    CHECK(twice.error().find("at most one scene per canvas") != std::string::npos);

    // But a *different* scene cueing the same canvas is the three-canvas case and is fine.
    CHECK(apply(f.project, AddSceneLink{
        SceneLink{Id::generate(), f.spare_scene, another_vertical.id}, 0}).has_value());
}

LTEST_CASE(a_link_can_only_start_on_main) {
    // The operator's rule: linking is a secondary canvas's function. A secondary scene's link
    // list answers "which Main scenes bring me up", and Main's own rows carry no link control
    // at all. So the source of a link is always a Main scene -- enforced here and not only in
    // the panel, because it is what a link *is*.
    Fixture f;
    const Id vertical_canvas = f.project.scene(f.vertical_scene)->canvas;
    REQUIRE(f.project.canvases.size() >= 2);
    REQUIRE(f.project.canvases.front().id != vertical_canvas);

    // A third canvas, so there is somewhere for a non-Main link to point that is not Main and
    // not the canvas it starts on -- otherwise the same-canvas rule would catch it first and
    // the test would pass for the wrong reason.
    Canvas third;
    third.id = Id::generate();
    third.name = "Ticker";
    REQUIRE(apply(f.project, AddCanvas{third, f.project.canvases.size()}).has_value());
    Scene on_third;
    on_third.id = Id::generate();
    on_third.name = "Ticker A";
    on_third.canvas = third.id;
    REQUIRE(apply(f.project, AddScene{on_third, 0}).has_value());

    const auto sideways = apply(
        f.project, AddSceneLink{SceneLink{Id::generate(), f.vertical_scene, on_third.id}, 0});
    CHECK(!sideways.has_value());
    CHECK(sideways.error().find("is not on") != std::string::npos);

    // The same link from Main is fine, which is what makes the refusal above about the source
    // and not about the destination.
    CHECK(apply(f.project, AddSceneLink{SceneLink{Id::generate(), f.spare_scene, on_third.id}, 0})
              .has_value());
}

LTEST_CASE(a_cue_and_a_safety_net_cannot_be_the_same_scene) {
    // ADR-0027 rule 5, enforced from both sides: the order the operator does the two steps in
    // must not decide whether the forbidden state is reachable.
    Fixture f;
    const Id vertical_canvas = f.project.scene(f.vertical_scene)->canvas;

    // The scene is already cued by the fixture's link, so it cannot become the safety net.
    const auto net = apply(f.project,
                           SetCanvasEmergencyScene{vertical_canvas, f.vertical_scene});
    CHECK(!net.has_value());
    CHECK(net.error().find("cannot also be the safety net") != std::string::npos);

    // And the other way round: drop the link, make it the net, then try to cue it again.
    REQUIRE(apply(f.project, RemoveSceneLink{f.existing_link}).has_value());
    REQUIRE(apply(f.project, SetCanvasEmergencyScene{vertical_canvas, f.vertical_scene})
                .has_value());
    const auto cue = apply(f.project,
                           AddSceneLink{SceneLink{Id::generate(), f.scene, f.vertical_scene}, 0});
    CHECK(!cue.has_value());
    CHECK(cue.error().find("emergency scene") != std::string::npos);
}

LTEST_CASE(removing_a_scene_takes_its_links_and_undo_brings_them_back) {
    Fixture f;
    const Project before = f.project;
    REQUIRE(f.project.scene_links.size() == 1);

    auto removed = apply(f.project, RemoveScene{f.vertical_scene});
    REQUIRE_MSG(removed.has_value(), "{}", removed.error());
    CHECK_MSG(f.project.scene_links.empty(),
              "a link was left pointing at a scene that is gone");

    auto undone = apply(f.project, removed->inverse);
    REQUIRE_MSG(undone.has_value(), "{}", undone.error());
    CHECK_MSG(f.project == before, "undoing the removal did not restore the link");
}

LTEST_CASE(removing_a_source_leaves_its_nodes_pointing_at_nothing_and_says_so) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetNodeSource{f.scene, f.loose, f.existing_source}).has_value());
    REQUIRE(h.run(RemoveSource{f.existing_source}).has_value());

    CHECK_MSG(is_valid(h.project()), "a dangling source reference was treated as unrenderable");
    const auto problems = validate(h.project());
    CHECK(!problems.empty());
    CHECK(problems.front().severity == Severity::Warning);

    // And undoing brings the source back with the binding intact.
    REQUIRE(h.undo().has_value());
    CHECK(h.project().source(f.existing_source) != nullptr);
    CHECK(validate(h.project()).empty());
}

LTEST_CASE(a_canvas_cannot_be_given_an_impossible_size_or_rate) {
    Fixture f;
    const Project before = f.project;
    Canvas bad = f.project.canvases.front();
    bad.width = 0;
    CHECK(!apply(f.project, SetCanvas{bad}).has_value());

    bad = f.project.canvases.front();
    bad.fps = Fps{30, 0};
    CHECK(!apply(f.project, SetCanvas{bad}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(disconnected_graph_inputs_round_trip_and_remain_undoable) {
    Fixture f;
    DocumentHistory h{f.project};
    auto video = *h.project().encode(f.existing_encode);
    auto audio = *h.project().audio_encode(f.existing_audio_encode);
    auto output = h.project().outputs.front();
    video.canvas = Id::nil();
    audio.bus = Id::nil();
    output.encode = Id::nil();
    output.audio_encode.reset();
    REQUIRE(h.run(make_batch({SetEncode{video}, SetAudioEncode{audio}, SetOutput{output}},
                             "Disconnect inputs")));
    CHECK(is_valid(h.project()));
    CHECK(!validate(h.project()).empty());
    const auto back = project_from_json(project_to_json(h.project()));
    REQUIRE(back);
    CHECK(back->project.encodes == h.project().encodes);
    CHECK(back->project.audio_encodes == h.project().audio_encodes);
    CHECK(back->project.outputs == h.project().outputs);

    REQUIRE(h.undo());
    CHECK(h.project() == f.project);
    REQUIRE(h.redo());
    const auto disconnected = h.project();
    // Removing an incomplete node must still have a working Add inverse.
    for (const Command& c : {Command{RemoveOutput{output.id}}, Command{RemoveEncode{video.id}},
                            Command{RemoveAudioEncode{audio.id}}}) {
        REQUIRE(h.run(c));
        REQUIRE(h.undo());
        CHECK(h.project() == disconnected);
    }
    // Missing is intentional; non-nil unknown references remain refused at the boundary.
    video.canvas = Id::generate();
    audio.bus = Id::generate();
    output.encode = Id::generate();
    CHECK(!h.run(SetEncode{video}));
    CHECK(!h.run(SetAudioEncode{audio}));
    CHECK(!h.run(SetOutput{output}));
    auto deck = h.project().decks.front();
    deck.canvas = Id::nil();
    CHECK(!h.run(SetDeck{deck}));
    CHECK(h.project() == disconnected);
}

LTEST_CASE(an_output_needs_a_canvas_but_a_bus_does_not_need_its_listeners) {
    // The same line again, landing differently because the model does. An output with no canvas
    // has nothing to encode, which validate() calls an error, so the command refuses it. A bus
    // that something still routes to is only a dangling reference -- a warning -- so removing it
    // is allowed and the complaint comes later.
    Fixture f;
    const Project before = f.project;

    Output orphan;
    orphan.id = Id::generate();
    orphan.name = "Nowhere";
    orphan.encode = Id::generate();
    CHECK(!apply(f.project, AddOutput{orphan, 0}).has_value());
    CHECK(f.project == before);

    Encode broke = f.project.encodes.front();
    broke.encoder.bitrate_kbps = 0;
    CHECK(!apply(f.project, SetEncode{broke}).has_value());

    Output live = f.project.outputs.front();
    live.enabled = true;
    live.destination.url.clear();
    CHECK_MSG(!apply(f.project, SetOutput{live}).has_value(),
              "an output was enabled with nowhere to send to");
    CHECK(f.project == before);

    // Removing a bus an audio encode reads is refused, the same way removing an encode an
    // output sends is: a bitstream with nothing to encode has no meaning.
    CHECK_MSG(!apply(f.project, RemoveBus{f.encoded_bus}).has_value(),
              "a bus was removed out from under an audio encode");

    // Removing a bus a source node routes to is allowed, and reported afterwards.
    DocumentHistory h{f.project};
    Node mic = make_node(NodeKind::Source, "Mic");
    std::get<SourceData>(mic.data).audio.bus = f.existing_bus;
    REQUIRE(h.run(AddNode{f.scene, std::nullopt, 0, std::move(mic)}).has_value());
    REQUIRE(h.run(RemoveBus{f.existing_bus}).has_value());
    CHECK_MSG(is_valid(h.project()), "a dangling bus reference was treated as unrenderable");
    CHECK(!validate(h.project()).empty());
}

LTEST_CASE(removing_an_imported_track_prunes_incoming_bus_sends_and_undo_restores_them) {
    auto project = make_default_project("Imported mixer");
    Bus track;
    track.id = Id::generate();
    track.name = "OBS track 2";
    track.kind = BusKind::Track;
    Bus input;
    input.id = Id::generate();
    input.name = "Imported audio";
    input.kind = BusKind::Input;
    input.sends = {{project.buses.front().id, 0, false}, {track.id, -3, true}};
    project.buses.push_back(track);
    project.buses.push_back(input);
    DocumentHistory history{project};
    REQUIRE(history.run(RemoveBus{track.id}));
    CHECK(is_valid(history.project()));
    REQUIRE(history.project().bus(input.id));
    CHECK_EQ(history.project().bus(input.id)->sends.size(), 1u);
    REQUIRE(history.undo());
    CHECK(history.project() == project);
    REQUIRE(history.redo());
    CHECK(is_valid(history.project()));
}

LTEST_CASE(a_continuous_gesture_is_one_undo_step_that_returns_to_where_it_started) {
    // What a dragged fader needs: four hundred commands, one undo, and the undo lands on the
    // value the fader had before the drag -- not one notch back.
    Fixture f;
    DocumentHistory h{f.project};
    const Id bus = f.project.buses.front().id;
    const double before = f.project.buses.front().gain_db;

    const std::string gesture = std::format("bus.gain:{}", bus.to_string());
    for (int i = 0; i < 400; ++i) {
        Bus b = h.project().buses.front();
        b.gain_db = -60.0 + static_cast<double>(i) * 0.15;
        REQUIRE(h.run(SetBus{b}, gesture).has_value());
    }
    CHECK_EQ(h.undo_depth(), std::size_t{1});
    CHECK_NEAR(h.project().buses.front().gain_db, -60.0 + 399 * 0.15, 1e-9);

    REQUIRE(h.undo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, before, 1e-9);
    CHECK_EQ(h.undo_depth(), std::size_t{0});

    // And redo puts the whole drag back, not one step of it.
    REQUIRE(h.redo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, -60.0 + 399 * 0.15, 1e-9);
}

LTEST_CASE(letting_go_ends_a_gesture_so_the_next_drag_is_its_own_step) {
    Fixture f;
    DocumentHistory h{f.project};
    const auto set_gain = [&](double db, bool coalesce) {
        Bus b = h.project().buses.front();
        b.gain_db = db;
        return coalesce ? h.run(SetBus{b}, "bus.gain") : h.run(SetBus{b});
    };

    REQUIRE(set_gain(-3.0, true).has_value());
    REQUIRE(set_gain(-6.0, true).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{1});

    h.end_gesture();
    REQUIRE(set_gain(-9.0, true).has_value());
    CHECK_MSG(h.undo_depth() == 2, "a second drag of the same fader merged into the first");

    // One undo per drag, each landing where that drag began.
    REQUIRE(h.undo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, -6.0, 1e-9);
    REQUIRE(h.undo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, 0.0, 1e-9);
}

LTEST_CASE(an_ordinary_command_in_the_middle_of_a_gesture_breaks_it) {
    // Nothing in the UI should do this, but if something does, the two edits must not end up
    // sharing one undo step -- undoing a fader move would then also rename a scene.
    Fixture f;
    DocumentHistory h{f.project};
    Bus b = h.project().buses.front();
    b.gain_db = -4.0;
    REQUIRE(h.run(SetBus{b}, "bus.gain").has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{1});

    REQUIRE(h.run(RenameScene{f.scene, "Interrupted"}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{2});
    CHECK(h.open_gesture().empty());

    b = h.project().buses.front();
    b.gain_db = -8.0;
    REQUIRE(h.run(SetBus{b}, "bus.gain").has_value());
    CHECK_MSG(h.undo_depth() == 3, "a gesture merged across an unrelated command");

    REQUIRE(h.undo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, -4.0, 1e-9);
    CHECK_EQ(h.project().scenes.front().name, std::string{"Interrupted"});
}

LTEST_CASE(two_different_controls_never_share_a_gesture) {
    Fixture f;
    DocumentHistory h{f.project};
    Bus b = h.project().buses.front();
    b.gain_db = -2.0;
    REQUIRE(h.run(SetBus{b}, "bus.gain:a").has_value());
    b = h.project().buses.front();
    b.pan = -0.5;
    REQUIRE(h.run(SetBus{b}, "bus.pan:a").has_value());
    CHECK_MSG(h.undo_depth() == 2, "a pan drag merged into a gain drag");
}

LTEST_CASE(a_refused_command_does_not_open_or_close_a_gesture) {
    Fixture f;
    DocumentHistory h{f.project};
    Bus b = h.project().buses.front();
    b.gain_db = -5.0;
    REQUIRE(h.run(SetBus{b}, "bus.gain").has_value());

    // A bus with no channels is refused; the drag that is still in progress must survive it.
    Bus bad = h.project().buses.front();
    bad.channels = 0;
    CHECK(!h.run(SetBus{bad}, "bus.gain").has_value());
    CHECK_EQ(h.open_gesture(), std::string_view{"bus.gain"});

    b = h.project().buses.front();
    b.gain_db = -7.0;
    REQUIRE(h.run(SetBus{b}, "bus.gain").has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{1});
    REQUIRE(h.undo().has_value());
    CHECK_NEAR(h.project().buses.front().gain_db, 0.0, 1e-9);
}

LTEST_CASE(a_batch_is_one_undo_step_that_restores_everything_it_changed) {
    // What "Add a camera" needs: three commands, one undo, and the undo takes all three back.
    Fixture f;
    DocumentHistory h{f.project};
    const Project start = h.project();

    Source cam;
    cam.id = Id::generate();
    cam.name = "Desk Cam";
    cam.type = "source.v4l2";
    const Id cam_id = cam.id;

    Node node = make_node(NodeKind::Source, "Desk Cam");
    const Id node_id = node.id;

    REQUIRE(h.run(make_batch({AddSource{cam, f.project.sources.size()},
                              AddNode{f.scene, std::nullopt, 0, node},
                              SetNodeSource{f.scene, node_id, cam_id}},
                             "Add Desk Cam"))
                .has_value());

    CHECK_EQ(h.undo_depth(), std::size_t{1});
    CHECK_EQ(h.undo_description(), std::string{"Add Desk Cam"});
    CHECK(h.project().source(cam_id) != nullptr);
    const Node* placed = find_node(h.project().scenes[0].roots, node_id);
    REQUIRE(placed != nullptr);
    CHECK(std::get<SourceData>(placed->data).source.value() == cam_id);

    REQUIRE(h.undo().has_value());
    CHECK_MSG(h.project() == start, "undoing a batch did not restore the project exactly");
    CHECK(h.project().source(cam_id) == nullptr);

    REQUIRE(h.redo().has_value());
    CHECK(h.project().source(cam_id) != nullptr);
}

LTEST_CASE(a_batch_that_cannot_finish_leaves_nothing_behind) {
    // The property that makes a batch worth having rather than three calls: a refused member
    // must take the applied ones with it. Otherwise "add a camera" could leave a source with no
    // source node, which is exactly the orphan this exists to prevent.
    Fixture f;
    const Project before = f.project;

    Source cam;
    cam.id = Id::generate();
    cam.name = "Doomed";
    cam.type = "source.v4l2";

    const Id nowhere = Id::generate();
    const auto r = apply(f.project, make_batch({AddSource{cam, f.project.sources.size()},
                                                RenameScene{nowhere, "no such scene"}},
                                               "Add Doomed"));
    CHECK_MSG(!r.has_value(), "a batch with an impossible member was accepted");
    CHECK_MSG(f.project == before, "a refused batch left part of itself behind");
    CHECK_MSG(f.project.source(cam.id) == nullptr, "the source from a refused batch survived");

    // And the error says which member failed, not just that something did.
    CHECK(r.error().contains("scene.rename"));
}

LTEST_CASE(an_empty_batch_and_a_nested_one_are_refused) {
    Fixture f;
    const Project before = f.project;

    CHECK(!apply(f.project, make_batch({}, "Nothing")).has_value());
    CHECK_EQ(f.project, before);

    const Command inner = make_batch({RenameScene{f.scene, "Inner"}}, "Inner");
    const auto nested = apply(f.project, make_batch({inner}, "Outer"));
    CHECK_MSG(!nested.has_value(), "a batch inside a batch was accepted");
    CHECK_EQ(f.project, before);
}

LTEST_CASE(two_batches_built_separately_from_equal_commands_are_equal) {
    // The body is shared through a pointer, so identity comparison would make the round-trip and
    // undo/redo property tests fail for reasons that have nothing to do with the document.
    const Command a = make_batch({RenameScene{Id::nil(), "x"}}, "Same");
    const Command b = make_batch({RenameScene{Id::nil(), "x"}}, "Same");
    CHECK(a == b);
    CHECK(!(a == make_batch({RenameScene{Id::nil(), "y"}}, "Same")));
    CHECK(!(a == make_batch({RenameScene{Id::nil(), "x"}}, "Different")));
}

LTEST_CASE(project_settings_are_undoable_and_bounded) {
    Fixture f;
    DocumentHistory h{f.project};
    Settings bad = f.project.settings;
    bad.sample_rate = 0;
    CHECK(!h.run(SetProjectSettings{bad}).has_value());

    Settings good = f.project.settings;
    good.sample_rate = 44100;
    REQUIRE(h.run(SetProjectSettings{good}).has_value());
    CHECK_EQ(h.project().settings.sample_rate, 44100);
    REQUIRE(h.undo().has_value());
    CHECK_EQ(h.project().settings.sample_rate, 48000);
}

LTEST_MAIN("command")

LTEST_CASE(replay_can_be_enabled_without_a_network_address) {
    Fixture f;
    auto replay = f.project.outputs.front();
    replay.destination.kind = DestinationKind::Replay;
    replay.destination.url.clear();
    replay.enabled = true;
    const auto applied = apply(f.project, SetOutput{replay});
    REQUIRE_MSG(applied.has_value(), "{}", applied.error());
    CHECK(f.project.outputs.front().enabled);
    CHECK(f.project.outputs.front().destination.url.empty());
}
