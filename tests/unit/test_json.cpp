// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The on-disk format: serialize -> parse -> serialize must be a fixed point, and a field that is
// present but wrong must be an error rather than a silent default.

#include "ldoc/json.hpp"

#include "ldoc/migrate.hpp"
#include "ltest.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <random>
#include <string>

using namespace ldoc;

namespace {

Project without_scenes(Project p) {
    p.scenes.clear();
    return p;
}

Node random_node(std::mt19937& rng, int depth) {
    const auto roll = rng() % 100;
    Node n;
    if (depth > 0 && roll < 30) {
        n = make_node(NodeKind::Group, std::format("g{}", rng() % 100));
        const int kids = 1 + static_cast<int>(rng() % 3);
        for (int i = 0; i < kids; ++i)
            n.children()->push_back(random_node(rng, depth - 1));
        std::get<GroupData>(n.data).isolate = (rng() % 2) == 0;
    } else if (roll < 50) {
        n = make_node(NodeKind::Source, std::format("l{}", rng() % 100));
        auto& ld = std::get<SourceData>(n.data);
        if (rng() % 2)
            ld.source = Id::generate();
        ld.audio.bus = Id::generate();
        ld.audio.gain_db = static_cast<double>(rng() % 2000) / 100.0 - 10.0;
        ld.audio.mute = (rng() % 2) == 0;
        if (rng() % 2)
            ld.audio.sends.push_back(Send{Id::generate(), -3.5, true});
    } else if (roll < 62) {
        n = make_node(NodeKind::Adjustment, "adj");
    } else if (roll < 74) {
        n = make_node(NodeKind::Mask, "mask");
        auto& m = std::get<MaskData>(n.data);
        m.kind = static_cast<MaskKind>(rng() % 3);
        m.invert = (rng() % 2) == 0;
        m.shape.emplace("radius", 12.5);
    } else if (roll < 88) {
        n = make_node(NodeKind::Reference, "ref");
        std::get<ReferenceData>(n.data).target = Id::generate();
        std::get<ReferenceData>(n.data).mode =
            (rng() % 2) ? ReferenceMode::Live : ReferenceMode::Snapshot;
    } else {
        n = make_node(NodeKind::Precomp, "pre");
        auto& p = std::get<PrecompData>(n.data);
        p.scene = Id::generate();
        p.size = Vec2{1080.0, 1920.0};
        p.color_space = static_cast<ColorSpace>(rng() % 5);
    }

    n.visible = (rng() % 4) != 0;
    n.locked = (rng() % 5) == 0;
    n.transform.position = Vec2{static_cast<double>(rng() % 4000) / 7.0, 1.0 / 3.0};
    n.transform.rotation_deg = static_cast<double>(rng() % 36000) / 100.0;
    n.transform.scale = Vec2{0.1 + static_cast<double>(rng() % 300) / 100.0, 2.0 / 7.0};
    n.layout.fit = static_cast<FitMode>(rng() % 6);
    n.layout.crop = CropRect{1.5, 2.25, 0.0, 99.0};
    n.layout.size = Vec2{1920.0, 1080.0};
    n.blend.mode = static_cast<BlendMode>(rng() % 25);
    n.blend.opacity = static_cast<double>(rng() % 101) / 100.0;
    n.blend.clip_to_below = (rng() % 2) == 0;

    if (rng() % 2) {
        Effect e;
        e.id = Id::generate();
        e.type = "effect.stroke";
        e.enabled = (rng() % 2) == 0;
        e.params.emplace("width", 3.25);
        e.params.emplace("count", std::int64_t{7});
        e.params.emplace("on", true);
        e.params.emplace("label", std::string{"a label"});
        e.params.emplace("offset", Vec2{1.0, -2.0});
        e.params.emplace("color", Rgba::from_srgb8(200, 100, 50, 255));
        e.params.emplace("link", Id::generate());
        e.params.emplace("nothing", std::monostate{});
        if (rng() % 2)
            e.mask = Id::generate();
        if (rng() % 2)
            e.blend = Blend{BlendMode::Multiply, 0.5, true, false};
        n.effects.push_back(std::move(e));
    }
    return n;
}

Scene random_scene(std::mt19937& rng, const Id& canvas_id) {
    Scene s;
    s.id = Id::generate();
    s.name = std::format("scene {}", rng() % 1000);
    s.canvas = canvas_id;
    const int roots = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < roots; ++i)
        s.roots.push_back(random_node(rng, 3));

    if (!s.roots.empty() && rng() % 2) {
        AudioOverride ov;
        ov.gain_db = -4.5;
        ov.mute = true;
        ov.bus = Id::generate();
        ov.sends = std::vector<Send>{Send{Id::generate(), 1.5, false}};
        s.audio_overrides.emplace(s.roots[0].id, ov);
    }
    return s;
}

Project random_project(std::mt19937& rng) {
    Project p = make_default_project(std::format("Show {}", rng() % 1000));
    p.generation = rng() % 100000;
    p.settings.sample_rate = (rng() % 2) ? 48000 : 44100;
    p.settings.extra.emplace("tuning", 1.0 / 3.0);

    Canvas vertical;
    vertical.id = Id::generate();
    vertical.name = "Vertical";
    vertical.width = 1080;
    vertical.height = 1920;
    vertical.fps = Fps::ntsc(60);
    vertical.color_space = ColorSpace::Rec2100Pq;
    p.canvases.push_back(vertical);

    Source src;
    src.id = Id::generate();
    src.name = "Camera";
    src.type = "source.v4l2";
    src.settings.emplace("device", std::string{"/dev/video2"});
    src.settings.emplace("width", std::int64_t{2560});
    src.settings.emplace("full_range", false);
    p.sources.push_back(src);

    Encode enc;
    enc.id = Id::generate();
    enc.name = "1440p60 12 Mbps";
    enc.canvas = p.canvases[0].id;
    enc.encoder.implementation = "nvenc";
    enc.encoder.bitrate_kbps = 12000;
    enc.encoder.extra.emplace("rc-lookahead", std::int64_t{20});
    enc.rescale = Vec2{1920.0, 1080.0};
    p.encodes.push_back(enc);

    Output out;
    out.id = Id::generate();
    out.name = "YouTube";
    out.encode = enc.id;
    out.destination.kind = DestinationKind::Rtmps;
    out.destination.url = "rtmps://a.rtmps.youtube.com/live2";
    out.destination.credential_ref = "youtube-main";
    out.destination.service = "youtube";
    out.enabled = true;
    p.outputs.push_back(out);

    p.buses[0].sends.push_back(Send{p.buses[1].id, -6.0, false});
    p.decks[0].preview_scene = p.scenes[0].id;

    const int extra_scenes = static_cast<int>(rng() % 3);
    for (int i = 0; i < extra_scenes; ++i)
        p.scenes.push_back(random_scene(rng, p.canvases[0].id));
    return p;
}

} // namespace

LTEST_CASE(the_default_project_round_trips) {
    const Project p = make_default_project("Show");
    const std::string text = project_to_json(p);

    const auto back = project_from_json(text);
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK(back->project == without_scenes(p));

    // The scene manifest comes back separately, because in memory a project just holds its
    // scenes; the manifest only exists so that opening one knows what to look for.
    REQUIRE(back->scenes.size() == p.scenes.size());
    CHECK(back->scenes[0].id == p.scenes[0].id);
    CHECK_EQ(back->scenes[0].name, p.scenes[0].name);
    CHECK_EQ(back->scenes[0].generation, p.generation);

    // Reattach the scenes and the bytes must be identical.
    Project rebuilt = back->project;
    rebuilt.scenes = p.scenes;
    CHECK_MSG(project_to_json(rebuilt) == text, "serialising what was read gave different bytes");
}

LTEST_CASE(replay_outputs_preserve_routes_and_graph_positions) {
    auto p = make_default_project("Backtrack");
    Encode video;
    video.id = Id::generate();
    video.canvas = p.canvases.front().id;
    p.encodes.push_back(video);
    AudioEncode audio;
    audio.id = Id::generate();
    audio.bus = p.buses.front().id;
    p.audio_encodes.push_back(audio);
    Output replay;
    replay.id = Id::generate();
    replay.name = "Backtrack";
    replay.encode = video.id;
    replay.audio_encode = audio.id;
    replay.destination.kind = DestinationKind::Replay;
    replay.destination.replay_seconds = 35;
    replay.destination.replay_memory_mb = 128;
    replay.position = {915.0, 425.0};
    p.outputs.push_back(replay);
    p.settings.extra["encode.graph.initialized"] = true;
    const auto back = project_from_json(project_to_json(p));
    REQUIRE(back);
    CHECK(back->project == without_scenes(p));
}

LTEST_CASE(serialisation_is_deterministic) {
    // A project directory is meant to be diffable. If the same project can produce two different
    // files, every save churns the diff and the format stops being reviewable.
    const Project p = make_default_project("Show");
    CHECK_EQ(project_to_json(p), project_to_json(p));
    const Scene& s = p.scenes[0];
    CHECK_EQ(scene_to_json(s, 3), scene_to_json(s, 3));
}

LTEST_CASE(a_scene_round_trips_with_everything_on_it) {
    std::mt19937 rng{4242u};
    const Scene s = random_scene(rng, Id::generate());
    const std::string text = scene_to_json(s);

    const auto back = scene_from_json(text);
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK_MSG(back->scene == s, "the scene changed on the way through");
    CHECK_EQ(scene_to_json(back->scene, back->generation), text);
}

LTEST_CASE(serialise_parse_serialise_is_a_fixed_point) {
    // The property the plan asks for. Over random projects, because the interesting failures are
    // in combinations nobody writes a case for.
    std::mt19937 rng{20261012u};
    for (int i = 0; i < 150; ++i) {
        const Project p = random_project(rng);

        const std::string once = project_to_json(p);
        const auto parsed = project_from_json(once);
        REQUIRE_MSG(parsed.has_value(), "iteration {}: {}", i, parsed.error());
        REQUIRE_MSG(parsed->project == without_scenes(p), "iteration {}: project model changed",
                    i);
        REQUIRE_MSG(parsed->scenes.size() == p.scenes.size(),
                    "iteration {}: the scene manifest changed size", i);
        for (std::size_t k = 0; k < p.scenes.size(); ++k) {
            REQUIRE_MSG(parsed->scenes[k].id == p.scenes[k].id,
                        "iteration {}: scene manifest entry {} changed", i, k);
            REQUIRE_MSG(parsed->scenes[k].generation == p.generation,
                        "iteration {}: scene manifest generation {} changed", i, k);
        }
        Project rebuilt = parsed->project;
        rebuilt.scenes = p.scenes;
        const std::string twice = project_to_json(rebuilt);
        REQUIRE_MSG(once == twice, "iteration {}: project bytes changed on re-serialisation", i);

        for (const Scene& s : p.scenes) {
            const std::string s1 = scene_to_json(s, 7);
            const auto sp = scene_from_json(s1);
            REQUIRE_MSG(sp.has_value(), "iteration {}: {}", i, sp.error());
            REQUIRE_MSG(sp->generation == 7u, "iteration {}: the generation stamp was lost", i);
            REQUIRE_MSG(scene_to_json(sp->scene, sp->generation) == s1,
                        "iteration {}: scene bytes changed", i);
            REQUIRE_MSG(sp->scene == s, "iteration {}: scene model changed", i);
        }
    }
}

LTEST_CASE(every_parameter_type_survives_including_the_ambiguous_ones) {
    Scene s;
    s.id = Id::generate();
    s.name = "params";
    Node n = make_node(NodeKind::Source, "l");

    const Id linked = Id::generate();
    Effect e;
    e.id = Id::generate();
    e.type = "effect.test";
    e.params.emplace("b", true);
    e.params.emplace("i", std::int64_t{-42});
    e.params.emplace("d", 1.0 / 3.0);
    e.params.emplace("whole_double", 3.0);
    e.params.emplace("s", std::string{"text"});
    e.params.emplace("v", Vec2{1.5, -2.5});
    e.params.emplace("c", Rgba{0.1, 0.2, 0.3, 0.4});
    e.params.emplace("link", linked);
    e.params.emplace("null", std::monostate{});
    // The trap: a *string* whose contents are a perfectly good ULID. If ids were written bare,
    // this would come back as an Id and the round trip would be a lie.
    e.params.emplace("looks_like_an_id", std::string{Id::generate().to_string()});
    n.effects.push_back(std::move(e));
    s.roots.push_back(std::move(n));

    const auto back = scene_from_json(scene_to_json(s));
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    const ParamMap& p = back->scene.roots[0].effects[0].params;

    CHECK(std::holds_alternative<bool>(p.at("b")));
    CHECK(std::holds_alternative<std::int64_t>(p.at("i")));
    CHECK(std::holds_alternative<double>(p.at("d")));
    CHECK_MSG(std::holds_alternative<double>(p.at("whole_double")),
              "a double with no fractional part came back as an integer");
    CHECK(std::holds_alternative<std::string>(p.at("s")));
    CHECK(std::holds_alternative<Vec2>(p.at("v")));
    CHECK(std::holds_alternative<Rgba>(p.at("c")));
    CHECK(std::holds_alternative<Id>(p.at("link")));
    CHECK(std::holds_alternative<std::monostate>(p.at("null")));
    CHECK_MSG(std::holds_alternative<std::string>(p.at("looks_like_an_id")),
              "a string that looks like an id was decoded as one");
    CHECK(std::get<Id>(p.at("link")) == linked);
    CHECK_EQ(std::get<double>(p.at("d")), 1.0 / 3.0);
    CHECK(back->scene == s);
}

LTEST_CASE(doubles_survive_exactly) {
    // Not "to within an epsilon". A position that shifts by one ulp per save walks.
    Scene s;
    s.id = Id::generate();
    s.name = "precision";
    Node n = make_node(NodeKind::Source, "l");
    n.transform.position = Vec2{1.0 / 3.0, 0.1 + 0.2};
    n.transform.rotation_deg = 359.99999999999994;
    n.blend.opacity = 0.8999999999999999;
    s.roots.push_back(std::move(n));

    const auto back = scene_from_json(scene_to_json(s));
    REQUIRE(back.has_value());
    const Node& got = back->scene.roots[0];
    CHECK_EQ(got.transform.position.x, 1.0 / 3.0);
    CHECK_EQ(got.transform.position.y, 0.1 + 0.2);
    CHECK_EQ(got.transform.rotation_deg, 359.99999999999994);
    CHECK_EQ(got.blend.opacity, 0.8999999999999999);
}

LTEST_CASE(a_field_that_is_present_but_wrong_is_an_error) {
    // Lenient about absent, strict about wrong. An absent key is an older build; a key of the
    // wrong type is corruption or a bad hand-edit, and silently reinterpreting it loses work.
    const Project p = make_default_project("Show");
    std::string text = project_to_json(p);

    const auto corrupt = [&](std::string from, std::string to) {
        std::string copy = text;
        const auto at = copy.find(from);
        REQUIRE_MSG(at != std::string::npos, "'{}' is not in the file", from);
        copy.replace(at, from.size(), to);
        return copy;
    };

    for (const auto& [from, to] : std::vector<std::pair<std::string, std::string>>{
             {"\"width\" : 1920", "\"width\" : \"wide\""},
             {"\"name\" : \"Show\"", "\"name\" : 7"},
             {"\"channels\" : 2", "\"channels\" : [2]"},
             {"\"sample_rate\" : 48000", "\"sample_rate\" : 4.5"},
             {"\"canvases\" :", "\"canvases\" : 3, \"ignored\" :"},
         }) {
        const auto r = project_from_json(corrupt(from, to));
        CHECK_MSG(!r.has_value(), "accepted a project with '{}' replaced by '{}'", from, to);
    }
}

LTEST_CASE(an_unknown_enum_value_is_an_error_not_a_silent_default) {
    // A blend mode this build does not know must not quietly become Normal: that is a silent
    // change to how the show looks, written back to disk on the next save.
    Scene s;
    s.id = Id::generate();
    s.name = "s";
    s.roots.push_back(make_node(NodeKind::Source, "l"));
    std::string text = scene_to_json(s);
    const auto at = text.find("\"mode\" : \"normal\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::string{"\"mode\" : \"normal\""}.size(),
                 "\"mode\" : \"quantum-burn\"");

    const auto r = scene_from_json(text);
    CHECK_MSG(!r.has_value(), "an unknown blend mode was accepted");
    CHECK(r.error().find("quantum-burn") != std::string::npos);
}

LTEST_CASE(an_absent_field_takes_the_code_default) {
    // Forward compatibility in the other direction: a file written before a field existed still
    // opens, with the default this build would have written.
    const auto r = scene_from_json(R"({
      "schema_version": 1,
      "id": "00000000000000000000000001",
      "name": "minimal",
      "roots": [ { "id": "00000000000000000000000002", "kind": "layer" } ]
    })");
    REQUIRE_MSG(r.has_value(), "{}", r.error());
    CHECK_EQ(r->scene.roots.size(), std::size_t{1});
    const Node& n = r->scene.roots[0];
    CHECK(n.visible);
    CHECK(!n.locked);
    CHECK_EQ(n.blend.opacity, 1.0);
    CHECK(n.blend.mode == BlendMode::Normal);
    CHECK(n.kind() == NodeKind::Source);
    CHECK(n.transform.scale == (Vec2{1.0, 1.0}));
}

LTEST_CASE(malformed_and_hostile_input_is_rejected_with_a_message) {
    for (const std::string& bad : {
             std::string{""},
             std::string{"not json"},
             std::string{"[]"},
             std::string{"{}"},
             std::string{R"({"schema_version": 1})"},
             std::string{R"({"schema_version": 0, "id": "00000000000000000000000001"})"},
             std::string{R"({"schema_version": 99, "id": "00000000000000000000000001"})"},
             std::string{R"({"schema_version": 1, "id": "not-an-id"})"},
             std::string{R"({"schema_version": 1, "id": 7})"},
             std::string{R"({"schema_version": 1, "id": "00000000000000000000000001",
                             "roots": [ {"id": "00000000000000000000000002",
                                         "kind": "sorcery"} ]})"},
             std::string{R"({"schema_version": 1, "id": "00000000000000000000000001",
                             "roots": [ {"kind": "reference"} ]})"},
             std::string{R"({"schema_version": 1, "id": "00000000000000000000000001",
                             "roots": "not an array"})"},
             std::string{R"({"schema_version": 1, "id": "00000000000000000000000001",
                             "audio_overrides": {"nope": {}}})"},
         }) {
        const auto r = scene_from_json(bad);
        CHECK_MSG(!r.has_value(), "accepted: {}", bad.substr(0, 60));
        if (!r.has_value())
            CHECK_MSG(!r.error().empty(), "rejected with an empty message: {}",
                      bad.substr(0, 60));
    }
}

LTEST_CASE(a_newer_schema_version_is_refused_rather_than_partly_read) {
    Project p = make_default_project("Show");
    p.schema_version = kSchemaVersion + 1;
    const auto r = project_from_json(project_to_json(p));
    CHECK(!r.has_value());
    CHECK(r.error().find("newer than this build") != std::string::npos);
}

LTEST_CASE(the_file_format_has_nowhere_to_put_a_stream_key) {
    // A guard, not a round-trip check. If a field is ever added that could hold a secret, this
    // fails -- which is the point. The project directory is meant to be diffable and shareable,
    // and those two things are only safe together while this stays true.
    Project p = make_default_project("Show");
    Encode e;
    e.id = Id::generate();
    e.name = "1080p60 6 Mbps";
    e.canvas = p.canvases[0].id;
    p.encodes.push_back(e);

    Output o;
    o.id = Id::generate();
    o.name = "Twitch";
    o.encode = e.id;
    o.destination.kind = DestinationKind::Rtmp;
    o.destination.url = "rtmp://ingest.example/app";
    o.destination.credential_ref = "twitch-main";
    p.outputs.push_back(o);

    const std::string text = project_to_json(p);
    for (std::string_view banned : {"\"key\"", "\"stream_key\"", "\"secret\"", "\"password\"",
                                    "\"token\"", "\"credentials\"", "\"auth\""})
        CHECK_MSG(text.find(banned) == std::string::npos,
                  "the project format has a field called {}", banned);
    CHECK(text.find("credential_ref") != std::string::npos);
    CHECK(text.find("twitch-main") != std::string::npos);
}

LTEST_CASE(assets_round_trip) {
    std::vector<Asset> assets;
    Asset a;
    a.id = Id::generate();
    a.path = "assets/stinger.webm";
    a.sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a.bytes = 4294967296ULL; // deliberately past 32 bits
    a.relocation_hints.push_back("/home/someone/Videos/stinger.webm");
    assets.push_back(a);

    const std::string text = assets_to_json(assets);
    const auto back = assets_from_json(text);
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK(*back == assets);
    CHECK_EQ(assets_to_json(*back), text);
    CHECK_EQ(back->front().bytes, 4294967296ULL);
}


LTEST_CASE(version_3_audio_settings_become_shared_audio_encodes) {
    // The v3 -> v4 bump. Audio used to be four string keys inside each video encode, which
    // could say "this encode carries AAC at 320" but could not say that several destinations
    // receive the *same* AAC bitstream. Two encodes that asked for identical audio were always
    // one stream; after the migration they say so.
    Project p = make_default_project("audio migration");
    p.scenes.clear();
    p.schema_version = 3;

    const Id canvas = p.canvases.front().id;
    const auto program = std::find_if(p.buses.begin(), p.buses.end(), [](const Bus& b) {
        return b.kind == BusKind::Program;
    });
    REQUIRE(program != p.buses.end());

    const auto make_encode = [&](const char* name, const char* codec, std::int64_t kbps,
                                 std::int64_t vod) {
        Encode e;
        e.id = Id::generate();
        e.name = name;
        e.canvas = canvas;
        e.encoder.extra["audio.codec"] = std::string{codec};
        e.encoder.extra["audio.bitrate_kbps"] = kbps;
        e.encoder.extra["audio.track"] = std::int64_t{1};
        e.encoder.extra["audio.vod_track"] = vod;
        p.encodes.push_back(e);
        return e.id;
    };
    // Two that match, one that does not.
    const Id twitch = make_encode("1080p60 6 Mbps", "aac", 320, 0);
    const Id youtube = make_encode("1080p60 8 Mbps", "aac", 320, 0);
    const Id vertical = make_encode("1080x1920 4 Mbps", "aac", 160, 0);

    const auto make_output = [&](const char* name, const Id& encode) {
        Output o;
        o.id = Id::generate();
        o.name = name;
        o.encode = encode;
        p.outputs.push_back(o);
        return o.id;
    };
    const Id out_twitch = make_output("Twitch", twitch);
    const Id out_youtube = make_output("YouTube", youtube);
    const Id out_vertical = make_output("TikTok", vertical);

    const auto loaded = project_from_json(project_to_json(p));
    REQUIRE_MSG(loaded.has_value(), "{}", loaded.error());
    Project back = loaded->project;
    const Migration m = migrate(back, loaded->legacy_outputs);
    CHECK_EQ(m.from, 3);
    CHECK_EQ(back.schema_version, kSchemaVersion);

    // Identical audio deduplicates; different audio does not.
    CHECK_EQ(back.audio_encodes.size(), std::size_t{2});

    const auto audio_of = [&](const Id& output) -> std::optional<Id> {
        const auto it = std::find_if(back.outputs.begin(), back.outputs.end(),
                                     [&output](const Output& o) { return o.id == output; });
        return it == back.outputs.end() ? std::nullopt : it->audio_encode;
    };
    const auto shared = audio_of(out_twitch);
    REQUIRE_MSG(shared.has_value(), "the migrated output carries no audio encode");
    CHECK_MSG(audio_of(out_youtube) == shared,
              "two outputs with identical audio did not end up sharing one encode");
    CHECK_MSG(audio_of(out_vertical) != shared,
              "an output with different audio was folded into the shared encode");

    const AudioEncode* a = back.audio_encode(*shared);
    REQUIRE(a != nullptr);
    CHECK_EQ(a->codec, std::string{"aac"});
    CHECK_EQ(a->bitrate_kbps, 320);
    CHECK_MSG(a->bus == program->id, "the audio encode was not attached to the program bus");

    // The keys the migration read are gone, rather than left behind to be read twice.
    for (const Encode& e : back.encodes) {
        for (const char* key : {"audio.codec", "audio.bitrate_kbps", "audio.track",
                                "audio.vod_track"}) {
            CHECK_MSG(e.encoder.extra.find(key) == e.encoder.extra.end(),
                      "'{}' survived the migration", key);
        }
    }

    // And the result is a project that validates and round-trips.
    const auto problems = validate(back);
    const bool fatal = std::any_of(problems.begin(), problems.end(), [](const Problem& pr) {
        return pr.severity == Severity::Error;
    });
    CHECK_MSG(!fatal, "the migrated project does not validate");
    const auto again = project_from_json(project_to_json(back));
    REQUIRE_MSG(again.has_value(), "{}", again.error());
    CHECK_EQ(again->project.audio_encodes, back.audio_encodes);
    CHECK_EQ(again->project.outputs, back.outputs);
}

LTEST_CASE(a_vod_track_migrates_onto_the_vod_bus) {
    // "audio.vod_track" non-zero meant the copyright-safe mix, which is a different bus and so
    // must not share a bitstream with the program one even at identical settings.
    Project p = make_default_project("vod migration");
    p.scenes.clear();
    p.schema_version = 3;

    const auto vod = std::find_if(p.buses.begin(), p.buses.end(),
                                  [](const Bus& b) { return b.kind == BusKind::Vod; });
    REQUIRE_MSG(vod != p.buses.end(), "the default project no longer has a VOD bus");

    Encode e;
    e.id = Id::generate();
    e.name = "1080p60 8 Mbps";
    e.canvas = p.canvases.front().id;
    e.encoder.extra["audio.codec"] = std::string{"aac"};
    e.encoder.extra["audio.bitrate_kbps"] = std::int64_t{160};
    e.encoder.extra["audio.vod_track"] = std::int64_t{2};
    p.encodes.push_back(e);

    const auto loaded = project_from_json(project_to_json(p));
    REQUIRE_MSG(loaded.has_value(), "{}", loaded.error());
    Project back = loaded->project;
    static_cast<void>(migrate(back, loaded->legacy_outputs));

    REQUIRE(back.audio_encodes.size() == std::size_t{1});
    CHECK_MSG(back.audio_encodes.front().bus == vod->id,
              "a VOD track did not migrate onto the VOD bus");
}

LTEST_MAIN("json")
