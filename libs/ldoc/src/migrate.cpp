// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/migrate.hpp"

#include <map>
#include <algorithm>
#include <format>
#include <variant>

namespace ldoc {
namespace {

/// Version 1 -> 2, the bump ADR-0027, ADR-0028 and ADR-0029 share.
///
/// What a version 1 project looks like, and what each part becomes:
///
///  - Scenes had no canvas. Every one is assigned the **first** canvas, which is the only one
///    the shell has ever worked against, so this is a restatement of what was already true
///    rather than a guess (ADR-0027).
///  - Nodes could carry `canvas_overrides`. They are gone with the shared scenes they existed
///    for. The reader counts them on the way past and the caller reports the count; nothing is
///    dropped in silence.
///  - Outputs carried a canvas and encoder settings of their own. Each distinct pair becomes
///    one `Encode` and every output asking for that same pair points at it (ADR-0029). Sharing
///    rather than one encode per output, because the two are indistinguishable in the document
///    and only one of them is true about the GPU: three outputs that were configured
///    identically really were three encodes before, and really are one now.
///
/// Outputs that asked for the same canvas and the same encoder settings become one encode.
void outputs_to_encodes(Project& p, const std::vector<LegacyOutputEncode>& legacy,
                        std::vector<Problem>& out) {
    if (legacy.empty())
        return;

    struct Made {
        Id canvas;
        EncoderConfig encoder;
        Id encode;
    };
    std::vector<Made> made;

    for (const LegacyOutputEncode& old : legacy) {
        const auto it = std::find_if(made.begin(), made.end(), [&old](const Made& m) {
            return m.canvas == old.canvas && m.encoder == old.encoder;
        });
        Id encode_id;
        if (it != made.end()) {
            encode_id = it->encode;
        } else {
            Encode e;
            e.id = Id::generate();
            e.canvas = old.canvas;
            e.encoder = old.encoder;
            e.name = default_encode_name(p, e);
            encode_id = e.id;
            made.push_back(Made{old.canvas, old.encoder, e.id});
            p.encodes.push_back(std::move(e));
        }
        for (Output& o : p.outputs) {
            if (o.id == old.output)
                o.encode = encode_id;
        }
    }

    out.push_back(Problem{Severity::Warning,
                          std::format("{} output(s) from an older format now share {} encode(s); "
                                      "an encode is a document object of its own and outputs "
                                      "name one",
                                      legacy.size(), p.encodes.size()),
                          p.id});
}

void to_v2(Project& p, const std::vector<LegacyOutputEncode>& legacy,
           std::vector<Problem>& out) {
    outputs_to_encodes(p, legacy, out);

    if (p.canvases.empty()) {
        // Nothing to assign scenes to. validate() will call every scene an error, which is the
        // right outcome and a more useful message than anything invented here would be.
        if (!p.scenes.empty())
            out.push_back(Problem{Severity::Error,
                                  std::format("this project is from an older format and has {} "
                                              "scene(s) but no canvas to put them on",
                                              p.scenes.size()),
                                  p.id});
        return;
    }

    const Id first = p.canvases.front().id;
    std::size_t assigned = 0;
    for (Scene& s : p.scenes) {
        if (!s.canvas.is_nil())
            continue;
        s.canvas = first;
        ++assigned;
    }
    if (assigned > 0)
        out.push_back(Problem{Severity::Warning,
                              std::format("{} scene(s) from an older format were assigned to the "
                                          "canvas '{}', which is the one they were authored "
                                          "against",
                                          assigned, p.canvases.front().name),
                              p.canvases.front().id});
}

/// Lay the graph out in columns, so a migrated project opens readable rather than piled at the
/// origin. Only touches nodes that have no position of their own.
void place_on_graph(Project& p) {
    // Matching the Encode workspace's three columns; the left rail is not placed, because
    // canvases and buses are stacked there in order rather than positioned.
    constexpr double kEncodeColumn = 520.0;
    constexpr double kOutputColumn = 1000.0;
    constexpr double kRowHeight = 150.0;
    constexpr double kTop = 60.0;

    double y = kTop;
    for (Encode& e : p.encodes) {
        if (e.position == Vec2{})
            e.position = Vec2{kEncodeColumn, y};
        y += kRowHeight;
    }
    for (AudioEncode& e : p.audio_encodes) {
        if (e.position == Vec2{})
            e.position = Vec2{kEncodeColumn, y};
        y += kRowHeight;
    }
    y = kTop;
    for (Output& o : p.outputs) {
        if (o.position == Vec2{})
            o.position = Vec2{kOutputColumn, y};
        y += kRowHeight;
    }
}

/// Version 3 -> 4. Audio stops being four strings inside the video encode (ADR-0029 again).
///
/// A version 3 project said audio like this, per video encode:
///
///   encoder.extra["audio.codec"]        "aac" | "opus"
///   encoder.extra["audio.bitrate_kbps"] 160
///   encoder.extra["audio.track"]        1-6
///   encoder.extra["audio.vod_track"]    0 = off
///
/// That form can say "this video encode carries AAC at 320", but it cannot say that eight
/// destinations receive *the same* AAC bitstream -- which is what is actually happening, and
/// what the GPU and the muxer both already knew. So each distinct (bus, codec, bitrate) triple
/// becomes one AudioEncode and every output fed by a video encode that asked for it points at
/// that one, exactly as v1 -> v2 turned per-output encoder settings into shared Encodes.
///
/// Sharing rather than one audio encode per video encode: two indistinguishable encodes were
/// only ever one bitstream, and the deduplicated form is the one that is true.
void to_v4(Project& p, std::vector<Problem>& out) {
    if (p.encodes.empty())
        return;

    const auto bus_of_kind = [&p](BusKind kind) -> const Bus* {
        const auto it = std::find_if(p.buses.begin(), p.buses.end(),
                                     [kind](const Bus& b) { return b.kind == kind; });
        return it == p.buses.end() ? nullptr : &*it;
    };
    const Bus* program = bus_of_kind(BusKind::Program);
    const Bus* vod = bus_of_kind(BusKind::Vod);
    if (!program && !vod) {
        out.push_back(Problem{Severity::Warning,
                              "this project is in an older format and carried audio settings, "
                              "but it has no program bus to attach them to; its outputs will "
                              "go out silent until one is chosen",
                              p.id});
        for (Encode& e : p.encodes) {
            for (const char* key : {"audio.codec", "audio.bitrate_kbps", "audio.track",
                                    "audio.vod_track"})
                e.encoder.extra.erase(key);
        }
        return;
    }

    const auto text_of = [](const ParamMap& m, const char* key, std::string fallback) {
        const auto it = m.find(key);
        if (it == m.end())
            return fallback;
        const auto* s = std::get_if<std::string>(&it->second);
        return s && !s->empty() ? *s : fallback;
    };
    const auto number_of = [](const ParamMap& m, const char* key, std::int64_t fallback) {
        const auto it = m.find(key);
        if (it == m.end())
            return fallback;
        const auto* n = std::get_if<std::int64_t>(&it->second);
        return n ? *n : fallback;
    };

    struct Made {
        Id bus;
        std::string codec;
        std::int32_t bitrate;
        Id audio_encode;
    };
    std::vector<Made> made;
    // Which audio encode each video encode's outputs should now name.
    std::vector<std::pair<Id, Id>> by_video;

    for (Encode& e : p.encodes) {
        const std::string codec = text_of(e.encoder.extra, "audio.codec", "aac");
        const auto bitrate =
            static_cast<std::int32_t>(number_of(e.encoder.extra, "audio.bitrate_kbps", 160));
        // A non-zero VOD track meant "this one carries the copyright-safe mix".
        const bool wants_vod = number_of(e.encoder.extra, "audio.vod_track", 0) != 0;
        const Bus* bus = wants_vod && vod ? vod : program;
        if (!bus)
            bus = vod;

        for (const char* key :
             {"audio.codec", "audio.bitrate_kbps", "audio.track", "audio.vod_track"})
            e.encoder.extra.erase(key);

        const auto it = std::find_if(made.begin(), made.end(), [&](const Made& m) {
            return m.bus == bus->id && m.codec == codec && m.bitrate == bitrate;
        });
        if (it != made.end()) {
            by_video.emplace_back(e.id, it->audio_encode);
            continue;
        }
        AudioEncode a;
        a.id = Id::generate();
        a.name = std::format("{} {}", codec == "opus" ? "Opus" : "AAC", bitrate);
        a.bus = bus->id;
        a.codec = codec;
        a.bitrate_kbps = bitrate > 0 ? bitrate : 160;
        made.push_back(Made{bus->id, codec, bitrate, a.id});
        by_video.emplace_back(e.id, a.id);
        p.audio_encodes.push_back(std::move(a));
    }

    for (Output& o : p.outputs) {
        const auto it = std::find_if(by_video.begin(), by_video.end(),
                                     [&o](const auto& pair) { return pair.first == o.encode; });
        if (it != by_video.end())
            o.audio_encode = it->second;
    }

    out.push_back(Problem{
        Severity::Warning,
        std::format("audio settings on {} encode(s) from an older format now share {} audio "
                    "encode(s); an audio encode is a document object of its own and its "
                    "outputs name one",
                    p.encodes.size(), p.audio_encodes.size()),
        p.id});

    place_on_graph(p);
}


/// Version 4 -> 5. Geometry stops being canvas pixels and becomes fractions (ADR-0070).
///
/// A version 4 project put every node where it was in pixels, so a canvas changed from 1080p to
/// 4K left the whole scene in the lower-left quarter and the operator re-placed each source by
/// hand. Fractions mean the numbers already say the same thing at the new size, and `SetCanvas`
/// needs no rescaling pass at all.
///
/// What is divided, and by what:
///
///  - `Transform.position` and `Layout.size`, by the size of the space the node sits in -- the
///    scene's canvas for a root node, the containing group's surface for a nested one, which is
///    itself resolved from the canvas on the way down.
///  - `GroupData::size` and `PrecompData::size`, by that same space, because they *are* that
///    space for their children.
///  - Nothing else. `scale`, `rotation_deg`, `pivot`, `skew_deg` and `Layout.alignment` were
///    already unit-free, and `CropRect` stays in source pixels: a crop is a statement about the
///    incoming image, not about the canvas it is drawn onto.
///
/// A scene whose canvas is missing falls back to the first canvas, which is what v1 -> v2 does
/// with scenes that never had one. The division is exact arithmetic with one right answer, so a
/// migrated project draws the identical rectangles at the size it was authored at -- which is
/// what `test_golden`'s version 4 case asserts rather than taking on trust.
///
/// A scene is converted once, against its own canvas, even when a precomp draws it into a
/// surface of some other size. That is the size it was authored at, and it is the only one this
/// pass can know; a scene used as a precomp at two different sizes had the same ambiguity in
/// pixels, and converting it twice would mean two answers for one scene.
void nodes_to_fractions(Node& node, double space_w, double space_h) {
    node.transform.position.x /= space_w;
    node.transform.position.y /= space_h;
    node.layout.size.x /= space_w;
    node.layout.size.y /= space_h;

    // The child space in pixels, before this node's own size is converted: a group of 960x540
    // inside 1920x1080 is where its children were placed, and that is still true afterwards.
    double child_w = space_w;
    double child_h = space_h;
    if (auto* group = std::get_if<GroupData>(&node.data)) {
        if (group->size && group->size->x > 0.0 && group->size->y > 0.0) {
            child_w = group->size->x;
            child_h = group->size->y;
            group->size = Vec2{group->size->x / space_w, group->size->y / space_h};
        }
        for (Node& child : group->children)
            nodes_to_fractions(child, child_w, child_h);
        return;
    }
    if (auto* pre = std::get_if<PrecompData>(&node.data)) {
        if (pre->size.x > 0.0 && pre->size.y > 0.0)
            pre->size = Vec2{pre->size.x / space_w, pre->size.y / space_h};
    }
}

void to_fractions(Project& p, std::vector<Problem>& out) {
    if (p.scenes.empty())
        return;
    if (p.canvases.empty()) {
        out.push_back(Problem{Severity::Error,
                              std::format("this project is from an older format and has {} "
                                          "scene(s) placed in pixels, but no canvas to measure "
                                          "them against; their geometry cannot be converted",
                                          p.scenes.size()),
                              p.id});
        return;
    }

    const Canvas& fallback = p.canvases.front();
    std::size_t orphans = 0;
    for (Scene& scene : p.scenes) {
        const Canvas* canvas = nullptr;
        for (const Canvas& c : p.canvases) {
            if (c.id == scene.canvas)
                canvas = &c;
        }
        if (!canvas) {
            canvas = &fallback;
            ++orphans;
        }
        if (canvas->width <= 0 || canvas->height <= 0)
            continue;
        const auto w = static_cast<double>(canvas->width);
        const auto h = static_cast<double>(canvas->height);
        for (Node& root : scene.roots)
            nodes_to_fractions(root, w, h);
    }

    if (orphans > 0)
        out.push_back(Problem{Severity::Warning,
                              std::format("{} scene(s) name a canvas this project does not have; "
                                          "their geometry was converted against '{}'",
                                          orphans, fallback.name),
                              fallback.id});
}

/// Fold a text node's scale into the font size it should have been asked for.
///
/// A text source has no native pixel size -- it has a size it was asked to draw at -- so a scale
/// on a text node was always a magnified bitmap rather than larger type. Version 6 makes a handle
/// drag set the font size instead, which leaves projects written before it carrying the old
/// arrangement: one real node was 48px type at scale 10.55, drawn ten times its rendered size.
///
/// Converted here rather than left for the operator to find, because the alternative is a show
/// that looks soft until every text node has been visited by hand. The drawn size is unchanged:
/// the font size is multiplied by exactly the factor the scale is divided by.
///
/// The font size lives on the *source*, which several nodes may share, so it cannot be rewritten
/// per node: the first node to be converted would change the type under every other one. The
/// size is taken from the node that uses the source at the largest scale, and the rest keep a
/// scale relative to that -- smaller than 1 rather than larger, so they are downscaled, which
/// costs sharpness nowhere.
void to_v6(Project& p, std::vector<Problem>& out) {
    std::map<Id, double> largest;
    const auto survey = [&](auto&& self, const Node& node) -> void {
        if (const auto* group = std::get_if<GroupData>(&node.data))
            for (const Node& child : group->children)
                self(self, child);
        const auto* placed = std::get_if<SourceData>(&node.data);
        if (!placed || !placed->source)
            return;
        const Source* src = p.source(*placed->source);
        if (!src || src->type != "source.text")
            return;
        const double factor = std::max(std::abs(node.transform.scale.x),
                                       std::abs(node.transform.scale.y));
        if (!std::isfinite(factor) || factor <= 0.0)
            return;
        auto& seen = largest[*placed->source];
        seen = std::max(seen, factor);
    };
    for (const Scene& scene : p.scenes)
        for (const Node& root : scene.roots)
            survey(survey, root);

    std::size_t converted = 0;
    for (Source& src : p.sources) {
        const auto found = largest.find(src.id);
        if (found == largest.end() || std::abs(found->second - 1.0) < 1e-9)
            continue;
        const auto it = src.settings.find("size");
        const auto* stored =
            it == src.settings.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
        const double current = stored ? static_cast<double>(*stored) : 48.0;
        const double wanted = std::clamp(std::round(current * found->second), 1.0, 1024.0);
        if (std::abs(wanted - current) < 1.0)
            continue;
        src.settings["size"] = static_cast<std::int64_t>(wanted);
        const double realised = wanted / current;
        const auto rescale = [&](auto&& self, Node& node) -> void {
            if (auto* group = std::get_if<GroupData>(&node.data))
                for (Node& child : group->children)
                    self(self, child);
            const auto* placed = std::get_if<SourceData>(&node.data);
            if (!placed || placed->source != src.id)
                return;
            node.transform.scale = Vec2{node.transform.scale.x / realised,
                                        node.transform.scale.y / realised};
        };
        for (Scene& scene : p.scenes)
            for (Node& root : scene.roots)
                rescale(rescale, root);
        ++converted;
    }

    if (converted > 0)
        out.push_back(Problem{
            Severity::Warning,
            std::format("{} text source(s) were being magnified rather than drawn at size; their "
                        "font size now carries the scale, so they are drawn sharp at the size "
                        "they already appear",
                        converted),
            p.id});
}

} // namespace

void geometry_to_fractions(Project& project, std::vector<Problem>& problems) {
    to_fractions(project, problems);
}

/// Rescue clones of scenes that an older import left as dead placeholders.
///
/// OBS's source-clone can name a scene, which is ordinary nesting. The importer used to refuse
/// that case -- it required the target to be a non-scene -- so a clone of a scene landed as
/// `source.obs-unsupported`: a record of what OBS had, with no type that draws anything. A real
/// collection carried six of them.
///
/// The original OBS object is kept verbatim in `obs.original`, so the conversion the importer
/// would do today can be done now from what is already in the file. Only clones are touched, and
/// only when their target can be found by name: anything else keeps its placeholder, because a
/// placeholder is at least honest about having nothing to draw.
void clones_of_scenes(Project& p, std::vector<Problem>& out) {
    std::size_t rescued = 0;
    for (Source& src : p.sources) {
        if (src.type != "source.obs-unsupported")
            continue;
        const auto it = src.settings.find("obs.original");
        const auto* text = it == src.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
        if (!text)
            continue;
        // Read only the two strings this needs, rather than reviving the whole OBS reader here:
        // the type, and the name of the thing being cloned.
        const auto field = [&](std::string_view key) -> std::string {
            const auto quoted = std::string{"\""} + std::string{key} + "\"";
            auto at = text->find(quoted);
            if (at == std::string::npos)
                return {};
            at = text->find(':', at + quoted.size());
            if (at == std::string::npos)
                return {};
            at = text->find('"', at);
            if (at == std::string::npos)
                return {};
            const auto end = text->find('"', at + 1);
            return end == std::string::npos ? std::string{} : text->substr(at + 1, end - at - 1);
        };
        if (field("id") != "source-clone")
            continue;
        const auto wanted = field("clone");
        if (wanted.empty())
            continue;
        // By name, which is what OBS stores, and only when exactly one thing answers to it.
        const Scene* scene = nullptr;
        std::size_t matches = 0;
        for (const Scene& candidate : p.scenes)
            if (candidate.name == wanted) {
                scene = &candidate;
                ++matches;
            }
        const Source* input = nullptr;
        for (const Source& candidate : p.sources)
            if (candidate.name == wanted && candidate.id != src.id) {
                input = &candidate;
                ++matches;
            }
        if (matches != 1) {
            out.push_back(Problem{Severity::Warning,
                                  std::format("source clone '{}' names '{}', which is {}; it "
                                              "stays a placeholder until a target is chosen",
                                              src.name, wanted,
                                              matches == 0 ? "not in this project" : "ambiguous"),
                                  src.id});
            continue;
        }
        src.type = "source.clone";
        src.settings["target"] = scene ? scene->id.to_string() : input->id.to_string();
        // The settings a clone needs and a placeholder never had. Conservative defaults: the
        // operator's own OBS values for these are in `obs.original` if they ever matter, and
        // guessing wrong here would change what a restored source sounds like.
        if (!src.settings.contains("audio"))
            src.settings["audio"] = false;
        if (!src.settings.contains("noFilters"))
            src.settings["noFilters"] = false;
        ++rescued;
    }
    if (rescued != 0)
        out.push_back(Problem{Severity::Warning,
                              std::format("{} source clone(s) left unsupported by an older import "
                                          "now point at the scene or source they named",
                                          rescued),
                              Id::nil()});
}

Migration migrate(Project& project, const std::vector<LegacyOutputEncode>& legacy_outputs) {
    Migration m;
    m.from = project.schema_version;
    if (project.schema_version >= kSchemaVersion)
        return m;

    if (project.schema_version < 2)
        to_v2(project, legacy_outputs, m.problems);
    if (project.schema_version < 4)
        to_v4(project, m.problems);
    if (project.schema_version < 5)
        to_fractions(project, m.problems);
    if (project.schema_version < 6)
        to_v6(project, m.problems);
    if (project.schema_version < 7)
        clones_of_scenes(project, m.problems);
    // Version 8 added the timeline and graphic manifests. Nothing to convert: a version 7
    // project simply has neither, which reads correctly as a project with no timelines and no
    // graphics. The version exists so that an older build *refuses* a project that has them,
    // rather than opening it, ignoring the manifests and dropping them on its next save --
    // which would delete the operator's edits without saying so.
    project.schema_version = kSchemaVersion;
    return m;
}

} // namespace ldoc
