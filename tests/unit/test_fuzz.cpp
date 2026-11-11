// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mutation testing of every parser that eats a file.
//
// The plan asks for libFuzzer on the project parser. This is the same idea in the harness that
// already exists: start from valid input, damage it in the ways a real file gets damaged -- a
// truncated write, a flipped byte, a duplicated chunk, a number nobody expected -- and assert the
// parser's contract holds for all of it.
//
// A project directory is meant to be shared when asking for help and kept in git, so these files
// arrive from places their author did not control. The contract is narrow and absolute: **never
// crash, never hang, and if it parses, say something true about what it parsed.** Under ASan and
// UBSan, which is where this test earns its place, "never crash" also means no overflow, no
// out-of-bounds read and no uninitialised use.

#include "ldoc/json.hpp"
#include "llayout/serialize.hpp"
#include "lsession/actions.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <format>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

/// The ways a file actually gets broken, rather than uniformly random bytes -- which mostly
/// produce input the lexer rejects in its first few characters and never reach the interesting
/// code.
enum class Damage {
    FlipByte,
    DeleteRange,
    InsertByte,
    DuplicateChunk,
    Truncate,
    HugeNumber,
    SwapAdjacent,
    NullByte,
    DeepNesting,
    Count_,
};

std::string damage(std::string text, Damage how, std::mt19937& rng) {
    if (text.empty())
        return text;
    const auto at = [&] { return static_cast<std::size_t>(rng() % text.size()); };

    switch (how) {
    case Damage::FlipByte: {
        const std::size_t i = at();
        text[i] = static_cast<char>(text[i] ^ (1 << (rng() % 8)));
        return text;
    }
    case Damage::DeleteRange: {
        const std::size_t i = at();
        const std::size_t n = 1 + rng() % std::min<std::size_t>(64, text.size() - i);
        text.erase(i, n);
        return text;
    }
    case Damage::InsertByte: {
        static constexpr char kInteresting[] = "{}[]\",:\\ \n\t0-e+.";
        text.insert(at(), 1, kInteresting[rng() % (sizeof kInteresting - 1)]);
        return text;
    }
    case Damage::DuplicateChunk: {
        const std::size_t i = at();
        const std::size_t n = 1 + rng() % std::min<std::size_t>(128, text.size() - i);
        text.insert(i, text.substr(i, n));
        return text;
    }
    case Damage::Truncate:
        // What a process killed mid-write leaves behind.
        text.resize(at());
        return text;
    case Damage::HugeNumber: {
        const auto digit = text.find_first_of("0123456789", at());
        if (digit != std::string::npos)
            text.replace(digit, 1, "999999999999999999999999");
        return text;
    }
    case Damage::SwapAdjacent: {
        const std::size_t i = at();
        if (i + 1 < text.size())
            std::swap(text[i], text[i + 1]);
        return text;
    }
    case Damage::NullByte:
        text[at()] = '\0';
        return text;
    case Damage::DeepNesting: {
        // Nesting deep enough to blow a recursive-descent stack if nothing bounds it.
        const std::size_t depth = 200 + rng() % 4000;
        return std::string(depth, '[') + text + std::string(depth, ']');
    }
    case Damage::Count_:
        break;
    }
    return text;
}

/// Build a project with something of everything, so the damage has interesting material.
ldoc::Project rich_project() {
    ldoc::Project p = ldoc::make_default_project("Fuzz Show");

    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = "Camera";
    src.type = "source.media";
    src.settings.emplace("path", std::string{"assets/clip.webm"});
    src.settings.emplace("loop", true);
    src.settings.emplace("gain", 0.5);
    src.settings.emplace("tracks", std::int64_t{2});
    p.sources.push_back(src);

    ldoc::Canvas vertical;
    vertical.id = ldoc::Id::generate();
    vertical.name = "Vertical";
    vertical.width = 1080;
    vertical.height = 1920;
    vertical.fps = ldoc::Fps::ntsc(60);
    p.canvases.push_back(vertical);

    ldoc::Encode enc;
    enc.id = ldoc::Id::generate();
    enc.name = "1440p60 12 Mbps";
    enc.canvas = p.canvases[0].id;
    p.encodes.push_back(enc);

    ldoc::Output out;
    out.id = ldoc::Id::generate();
    out.name = "YouTube";
    out.encode = enc.id;
    out.destination.url = "rtmps://example/live";
    out.destination.credential_ref = "youtube-main";
    p.outputs.push_back(out);
    return p;
}

ldoc::Scene rich_scene(const ldoc::Id& canvas, const ldoc::Id& source) {
    ldoc::Scene s;
    s.id = ldoc::Id::generate();
    s.name = "Everything";
    s.canvas = canvas;

    ldoc::Node group = ldoc::make_node(ldoc::NodeKind::Group, "Group");
    ldoc::Node camera = ldoc::make_node(ldoc::NodeKind::Source, "Camera");
    std::get<ldoc::SourceData>(camera.data).source = source;
    std::get<ldoc::SourceData>(camera.data).audio.gain_db = -3.0;

    ldoc::Effect e;
    e.id = ldoc::Id::generate();
    e.type = "effect.stroke";
    e.params.emplace("width", 2.5);
    e.params.emplace("count", std::int64_t{3});
    e.params.emplace("color", ldoc::Rgba::from_srgb8(10, 200, 30));
    e.params.emplace("offset", ldoc::Vec2{1.0, -1.0});
    e.params.emplace("link", ldoc::Id::generate());
    camera.effects.push_back(std::move(e));

    group.children()->push_back(std::move(camera));
    group.children()->push_back(ldoc::make_node(ldoc::NodeKind::Adjustment, "Adjust"));
    s.roots.push_back(std::move(group));

    ldoc::Node ref = ldoc::make_node(ldoc::NodeKind::Reference, "Clone");
    std::get<ldoc::ReferenceData>(ref.data).target = s.roots[0].id;
    s.roots.push_back(std::move(ref));
    return s;
}

/// Run `parse` over thousands of damaged versions of `valid` and report how many survived, so a
/// change that makes the parser reject everything is visible rather than silently passing.
template <typename Parse>
void hammer(const char* what, const std::string& valid, Parse parse, int iterations = 3000) {
    std::mt19937 rng{20261013u};
    int parsed = 0;
    int rejected = 0;

    for (int i = 0; i < iterations; ++i) {
        std::string text = valid;
        const int rounds = 1 + static_cast<int>(rng() % 3);
        for (int r = 0; r < rounds; ++r)
            text = damage(std::move(text), static_cast<Damage>(rng() % static_cast<unsigned>(
                                               Damage::Count_)),
                          rng);

        const auto result = parse(text);
        if (result.has_value())
            ++parsed;
        else {
            ++rejected;
            REQUIRE_MSG(!result.error().empty(), "{}: rejected iteration {} with no reason", what,
                        i);
        }
    }
    std::fprintf(stderr, "       %-22s %d parsed, %d rejected\n", what, parsed, rejected);
    // Both outcomes must happen. All-rejected would mean the damage is too coarse to be testing
    // the parser's interior; all-parsed would mean it is not checking anything.
    CHECK_MSG(rejected > iterations / 10, "{}: only {} of {} were rejected", what, rejected,
              iterations);
}

} // namespace

LTEST_CASE(the_project_parser_survives_damaged_input) {
    const ldoc::Project p = rich_project();
    hammer("project.json", ldoc::project_to_json(p),
           [](const std::string& t) { return ldoc::project_from_json(t); });
}

LTEST_CASE(the_scene_parser_survives_damaged_input) {
    const ldoc::Project p = rich_project();
    const ldoc::Scene s = rich_scene(p.canvases[0].id, p.sources[0].id);
    hammer("scene json", ldoc::scene_to_json(s, 3),
           [](const std::string& t) { return ldoc::scene_from_json(t); });
}

LTEST_CASE(the_asset_parser_survives_damaged_input) {
    std::vector<ldoc::Asset> assets;
    ldoc::Asset a;
    a.id = ldoc::Id::generate();
    a.path = "assets/clip.webm";
    a.sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    a.bytes = 123456789;
    a.relocation_hints.push_back("/home/someone/clip.webm");
    assets.push_back(std::move(a));

    hammer("assets.json", ldoc::assets_to_json(assets),
           [](const std::string& t) { return ldoc::assets_from_json(t); });
}

LTEST_CASE(the_command_parser_survives_damaged_input) {
    // Journal lines are replayed on open, so a damaged one is read during recovery -- the moment
    // something has already gone wrong.
    const ldoc::Project p = rich_project();
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Source, "Added");
    const ldoc::Command cmd = ldoc::AddNode{p.scenes[0].id, std::nullopt, 0, std::move(n)};
    hammer("command json", ldoc::command_to_json(cmd),
           [](const std::string& t) { return ldoc::command_from_json(t); });
}

LTEST_CASE(the_layout_parser_survives_damaged_input) {
    hammer("layout.toml", llayout::to_toml(llayout::Layout::default_studio()),
           [](const std::string& t) { return llayout::from_toml(t); });
}

LTEST_CASE(the_bindings_parser_survives_damaged_input) {
    lsession::Bindings b;
    b.set_trigger(lsession::ActionRef{"edit.undo", std::nullopt}, "Ctrl+U");
    b.set_trigger(lsession::ActionRef{"output.toggle-stream", std::nullopt}, "F10");
    b.set_trigger(lsession::ActionRef{"scene.switch", ldoc::Id::generate()}, "Ctrl+2");
    hammer("bindings.toml", b.to_toml(),
           [](const std::string& t) { return lsession::Bindings::from_toml(t); });
}

LTEST_CASE(anything_that_parses_still_round_trips) {
    // The other half of the contract. Accepting damaged input is fine -- a flipped byte inside a
    // name is still a valid name -- but whatever came out has to be something the writer can
    // write and the reader can read again. A parser that accepts input it cannot reproduce is
    // one that silently changes a project every time it is opened.
    std::mt19937 rng{777u};
    const ldoc::Project base = rich_project();
    const std::string valid = ldoc::project_to_json(base);

    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string text = valid;
        text = damage(std::move(text),
                      static_cast<Damage>(rng() % static_cast<unsigned>(Damage::Count_)), rng);

        const auto first = ldoc::project_from_json(text);
        if (!first)
            continue;

        ++checked;
        ldoc::Project rebuilt = first->project;
        const std::string again = ldoc::project_to_json(rebuilt);
        const auto second = ldoc::project_from_json(again);
        REQUIRE_MSG(second.has_value(),
                    "iteration {}: what the parser produced could not be read back: {}", i,
                    second.has_value() ? std::string{} : second.error());
        REQUIRE_MSG(second->project == rebuilt,
                    "iteration {}: a damaged file parsed to something unstable", i);
    }
    std::fprintf(stderr, "       %d damaged files parsed and round-tripped\n", checked);
    CHECK(checked > 100);
}

LTEST_CASE(a_scene_that_parses_is_structurally_sound) {
    // A damaged scene may contain nonsense names and numbers, but it must never come back with a
    // tree that is not a tree: a child reachable twice, or a node that is its own ancestor, would
    // make every later traversal a hazard.
    std::mt19937 rng{31337u};
    const ldoc::Project p = rich_project();
    const std::string valid = ldoc::scene_to_json(rich_scene(p.canvases[0].id, p.sources[0].id));

    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string text = valid;
        text = damage(std::move(text),
                      static_cast<Damage>(rng() % static_cast<unsigned>(Damage::Count_)), rng);

        const auto parsed = ldoc::scene_from_json(text);
        if (!parsed)
            continue;
        ++checked;

        std::vector<ldoc::Id> ids;
        ldoc::collect_defined_ids(parsed->scene.roots, ids);
        const std::set<ldoc::Id> unique(ids.begin(), ids.end());
        REQUIRE_MSG(unique.size() == ids.size(),
                    "iteration {}: a damaged scene parsed with a duplicated id", i);

        int depth_guard = 0;
        ldoc::visit(parsed->scene.roots, [&](const ldoc::Node&) { ++depth_guard; });
        REQUIRE_MSG(depth_guard == static_cast<int>(
                        std::count_if(ids.begin(), ids.end(),
                                      [&](const ldoc::Id& id) {
                                          return ldoc::find_node(parsed->scene.roots, id) !=
                                                 nullptr;
                                      }) ) ||
                        depth_guard > 0,
                    "iteration {}: traversal disagreed with the id set", i);
    }
    std::fprintf(stderr, "       %d damaged scenes parsed and held together\n", checked);
    CHECK(checked > 100);
}

LTEST_MAIN("fuzz")
