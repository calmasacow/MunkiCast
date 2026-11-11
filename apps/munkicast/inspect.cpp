// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "inspect.hpp"

#include "ldoc/journal.hpp"
#include "ldoc/json.hpp"
#include "ldoc/migrate.hpp"
#include "ldoc/project_io.hpp"
#include "lcore/atomic_file.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace munkicast {
namespace {

void heading(const char* text) { std::printf("\n%s\n", text); }

void row(const char* label, const std::string& value) {
    std::printf("  %-22s %s\n", label, value.c_str());
}

std::string fps_text(const ldoc::Fps& f) {
    return f.den == 1 ? std::format("{}", f.num)
                      : std::format("{}/{} ({:.3f})", f.num, f.den, f.hz());
}

void print_tree(const std::vector<ldoc::Node>& nodes, const ldoc::Project& p, int depth) {
    for (const ldoc::Node& n : nodes) {
        std::string line(static_cast<std::size_t>(depth) * 2 + 4, ' ');
        line += std::format("{} {}", ldoc::to_string(n.kind()),
                            n.name.empty() ? "(unnamed)" : n.name);
        if (!n.visible)
            line += "  [hidden]";
        if (n.locked)
            line += "  [locked]";
        if (const auto* placed = std::get_if<ldoc::SourceData>(&n.data)) {
            if (placed->source) {
                const ldoc::Source* src = p.source(*placed->source);
                line += src ? std::format("  <- {}", src->name) : "  <- (missing source)";
            }
        }
        if (!n.effects.empty())
            line += std::format("  fx:{}", n.effects.size());
        if (n.blend.mode != ldoc::BlendMode::Normal || n.blend.opacity != 1.0)
            line += std::format("  {} {:.0f}%", ldoc::to_string(n.blend.mode),
                                n.blend.opacity * 100.0);
        std::printf("%s\n", line.c_str());
        if (const auto* kids = n.children())
            print_tree(*kids, p, depth + 1);
    }
}

} // namespace

int inspect_project(const fs::path& dir, bool verbose) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        std::fprintf(stderr, "munkicast: %s is not a project directory\n", dir.string().c_str());
        return 2;
    }

    const auto project_text = lc::read_file(dir / "project.json");
    if (!project_text) {
        std::fprintf(stderr, "munkicast: %s\n", project_text.error().c_str());
        return 2;
    }
    const auto file = ldoc::project_from_json(*project_text);
    if (!file) {
        std::fprintf(stderr, "munkicast: project.json could not be read: %s\n",
                     file.error().c_str());
        return 2;
    }

    ldoc::Project p = file->project;
    std::printf("=== %s ===\n", dir.string().c_str());
    row("name", p.name);
    row("schema version", std::format("{}", p.schema_version));
    row("generation", std::format("{}", p.generation));

    // Scenes are read individually so that one bad file is reported as one bad file, which is
    // the whole reason the format is a directory.
    std::vector<std::string> unreadable;
    for (const ldoc::SceneRef& ref : file->scenes) {
        const fs::path path = dir / "scenes" / std::format("{}.json", ref.id.to_string());
        const auto text = lc::read_file(path);
        if (!text) {
            unreadable.push_back(std::format("{} ({}) is missing", ref.name, ref.id.to_string()));
            continue;
        }
        auto scene = ldoc::scene_from_json(*text);
        if (!scene) {
            unreadable.push_back(std::format("{} ({}) will not parse: {}", ref.name,
                                             ref.id.to_string(), scene.error()));
            continue;
        }
        if (scene->generation != ref.generation)
            unreadable.push_back(std::format(
                "{} was written by generation {} but the project expects {}", ref.name,
                scene->generation, ref.generation));
        p.scenes.push_back(std::move(scene->scene));
    }
    if (const auto assets = lc::read_file(dir / "assets.json")) {
        if (const auto parsed = ldoc::assets_from_json(*assets))
            p.assets = *parsed;
    }

    // The same migration `open_project` runs, for the same reason: without it every project
    // written by an older build is reported as broken, which is a lie about the project and a
    // libel against the format. Nothing is written back -- this command only looks.
    const ldoc::Migration migrated = ldoc::migrate(p, file->legacy_outputs);
    if (migrated.happened())
        std::printf("  %-18s version %d, brought up to %d for this report only\n", "migration",
                    migrated.from, p.schema_version);

    heading("canvases");
    for (const ldoc::Canvas& c : p.canvases) {
        std::printf("  %-18s %dx%d @ %s  %s\n", c.name.c_str(), c.width, c.height,
                    fps_text(c.fps).c_str(), std::string{ldoc::to_string(c.color_space)}.c_str());
        // ADR-0025: what this canvas would broadcast if the show could not be supervised. The
        // single most useful thing to know before going live, and the easiest to forget.
        if (!c.emergency_scene) {
            std::printf("  %-18s no emergency scene: would fall back to black and silence\n", "");
        } else if (const ldoc::Scene* s = p.scene(*c.emergency_scene)) {
            std::printf("  %-18s emergency scene: %s\n", "", s->name.c_str());
            for (const ldoc::Problem& problem :
                 ldoc::check_emergency_scene(p, *c.emergency_scene))
                std::printf("  %-18s   ! %s\n", "", problem.message.c_str());
        } else {
            std::printf("  %-18s emergency scene is missing\n", "");
        }
    }

    heading("scenes");
    for (const ldoc::Scene& s : p.scenes) {
        int count = 0;
        ldoc::visit(s.roots, [&count](const ldoc::Node&) { ++count; });
        std::string tags;
        for (const ldoc::Deck& d : p.decks) {
            if (d.program_scene && *d.program_scene == s.id)
                tags += "  [program]";
            if (d.preview_scene && *d.preview_scene == s.id)
                tags += "  [preview]";
        }
        // Once, however many canvases point at it: a repeated tag reads as a bug in the report
        // rather than as information about the project.
        if (std::any_of(p.canvases.begin(), p.canvases.end(), [&s](const ldoc::Canvas& c) {
                return c.emergency_scene && *c.emergency_scene == s.id;
            }))
            tags += "  [emergency]";
        std::printf("  %-28s %d node(s)%s\n", s.name.c_str(), count, tags.c_str());
        if (verbose)
            print_tree(s.roots, p, 0);
    }

    heading("sources");
    if (p.sources.empty())
        std::printf("  (none)\n");
    for (const ldoc::Source& src : p.sources) {
        int uses = 0;
        for (const ldoc::Scene& s : p.scenes)
            ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
                if (placed && placed->source && *placed->source == src.id)
                    ++uses;
            });
        std::printf("  %-28s %-22s used by %d%s\n", src.name.c_str(), src.type.c_str(), uses,
                    ldoc::is_self_contained(src.type) ? "  [safe for emergency use]" : "");
    }

    // Before the outputs, because an output now names one of these and the pre-flight question
    // is how many encodes the GPU is being asked for (ADR-0029).
    heading("encodes");
    if (p.encodes.empty())
        std::printf("  (none)\n");
    for (const ldoc::Encode& e : p.encodes) {
        const ldoc::Canvas* c = p.canvas(e.canvas);
        int sent_to = 0;
        for (const ldoc::Output& o : p.outputs)
            sent_to += (o.encode == e.id) ? 1 : 0;
        std::printf("  %-28s %s %s %d kbps%s  -> %d destination(s)\n", e.name.c_str(),
                    c ? c->name.c_str() : "(missing canvas)", e.encoder.implementation.c_str(),
                    e.encoder.bitrate_kbps,
                    e.rescale ? std::format("  rescaled {:.0f}x{:.0f}", e.rescale->x,
                                            e.rescale->y).c_str()
                              : "",
                    sent_to);
    }
    std::printf("  %-28s %zu concurrent encode(s) asked of the GPU\n", "", p.encodes.size());

    heading("outputs");
    if (p.outputs.empty())
        std::printf("  (none)\n");
    for (const ldoc::Output& o : p.outputs) {
        const ldoc::Encode* e = p.encode(o.encode);
        std::printf("  %-20s %s -> %s%s\n", o.name.c_str(),
                    e ? e->name.c_str() : "(missing encode)",
                    o.destination.url.empty() ? "(nowhere)" : o.destination.url.c_str(),
                    o.enabled ? "  [enabled]" : "");
        // Never the credential itself: the format cannot hold one (ADR-0025's sibling reasoning
        // in ADR-0021), and this output is meant to be pasted into a bug report.
        if (!o.destination.credential_ref.empty())
            std::printf("  %-20s credential: %s (stored outside the project)\n", "",
                        o.destination.credential_ref.c_str());
    }

    const auto journal = ldoc::Journal::read(ldoc::journal_path(dir));
    if (journal && !journal->entries.empty()) {
        std::size_t pending = 0;
        for (const auto& e : journal->entries)
            pending += e.base_generation == p.generation ? 1 : 0;
        heading("journal");
        std::printf("  %zu entr(ies), %zu of them unsaved work that opening would recover\n",
                    journal->entries.size(), pending);
        if (journal->truncated_tail)
            std::printf("  the last entry was incomplete, which is what a crash mid-write "
                        "leaves\n");
    }

    const auto problems = ldoc::validate(p);
    heading("problems");
    for (const std::string& u : unreadable)
        std::printf("  error    %s\n", u.c_str());
    for (const ldoc::Problem& problem : problems)
        std::printf("  %-8s %s\n",
                    problem.severity == ldoc::Severity::Error ? "error" : "warning",
                    problem.message.c_str());
    if (problems.empty() && unreadable.empty())
        std::printf("  none\n");

    const bool bad = !unreadable.empty() ||
                     std::any_of(problems.begin(), problems.end(), [](const ldoc::Problem& x) {
                         return x.severity == ldoc::Severity::Error;
                     });
    return bad ? 1 : 0;
}

} // namespace munkicast
