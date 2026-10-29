// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ldoc {
namespace {

template <typename T, std::size_t N>
std::optional<T> lookup(const std::array<std::pair<T, std::string_view>, N>& table,
                        std::string_view s) {
    for (const auto& [value, name] : table)
        if (name == s)
            return value;
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string_view name_of(const std::array<std::pair<T, std::string_view>, N>& table, T v) {
    for (const auto& [value, name] : table)
        if (value == v)
            return name;
    return "unknown";
}

constexpr std::array<std::pair<BusKind, std::string_view>, 6> kBusKinds = {{
    {BusKind::Input, "input"},     {BusKind::Submix, "submix"},
    {BusKind::Monitor, "monitor"}, {BusKind::Track, "track"},
    {BusKind::Program, "program"}, {BusKind::Vod, "vod"},
}};

constexpr std::array<std::pair<DestinationKind, std::string_view>, 6> kDestinationKinds = {{
    {DestinationKind::Rtmp, "rtmp"}, {DestinationKind::Rtmps, "rtmps"},
    {DestinationKind::Srt, "srt"},   {DestinationKind::Whip, "whip"},
    {DestinationKind::File, "file"}, {DestinationKind::Replay, "replay"},
}};

/// Source types that cannot fault at the moment they are needed.
///
/// Everything absent from this list -- a capture device, a window or screen grab, a browser
/// engine, a network stream, anything unrecognised -- is treated as unsafe for an emergency
/// scene. A browser source that wedged and took the frontend with it is precisely the thing not
/// to put on the holding card.
constexpr std::array<std::string_view, 4> kSelfContainedSources = {
    "source.media",
    "source.image",
    "source.color",
    "source.text",
};

/// Source types with no audio of their own.
///
/// Stated as the silent list rather than the audible one so that an **unknown** type -- a newer
/// project file, or a source manifest this build has never seen (ADR-0004) -- answers "has
/// audio". That is the opposite default to is_self_contained above, and the asymmetry is
/// deliberate: there, inventing trust puts a source that can wedge onto the holding card, so
/// the safe guess is "no". Here the two mistakes are not equal either, but they point the other
/// way -- a fader for something silent does nothing, while a missing fader for something
/// audible is audio going to air with no way to mute it.
///
/// The membership comes from what OBS does type by type, because an operator arriving from OBS
/// knows this table already and a mixer that disagrees with it reads as a bug. The pair that
/// looks inconsistent -- window capture audible, whole-display capture not -- is inconsistent
/// in OBS too, and for a real reason: a window grab can carry that application's audio, while
/// a display grab is a video capture with no audio stream behind it. Desktop audio is a
/// separate source there, as it is here.
constexpr std::array<std::string_view, 5> kSilentSources = {
    "source.slideshow",
    "source.image",
    "source.text",
    "source.color",
    "source.screen",
};

std::string_view ref_kind_name(RefKind k) {
    switch (k) {
    case RefKind::Node:   return "node";
    case RefKind::Source: return "source";
    case RefKind::Scene:  return "scene";
    case RefKind::Bus:    return "bus";
    }
    return "thing";
}

/// What owns an id, for a message that names something the operator can find.
struct Owner {
    std::string_view kind;
    std::string name;
};

} // namespace

std::string_view to_string(BusKind k) { return name_of(kBusKinds, k); }
std::optional<BusKind> bus_kind_from_string(std::string_view s) { return lookup(kBusKinds, s); }

std::string_view to_string(DestinationKind k) { return name_of(kDestinationKinds, k); }
std::optional<DestinationKind> destination_kind_from_string(std::string_view s) {
    return lookup(kDestinationKinds, s);
}

const Canvas* Project::canvas(const Id& wanted) const {
    const auto it = std::find_if(canvases.begin(), canvases.end(),
                                 [&wanted](const Canvas& c) { return c.id == wanted; });
    return it == canvases.end() ? nullptr : &*it;
}

const Scene* Project::scene(const Id& wanted) const {
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&wanted](const Scene& s) { return s.id == wanted; });
    return it == scenes.end() ? nullptr : &*it;
}

Scene* Project::scene(const Id& wanted) {
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&wanted](const Scene& s) { return s.id == wanted; });
    return it == scenes.end() ? nullptr : &*it;
}

const Encode* Project::encode(const Id& wanted) const {
    const auto it = std::find_if(encodes.begin(), encodes.end(),
                                 [&wanted](const Encode& e) { return e.id == wanted; });
    return it == encodes.end() ? nullptr : &*it;
}

const AudioEncode* Project::audio_encode(const Id& wanted) const {
    const auto it = std::find_if(audio_encodes.begin(), audio_encodes.end(),
                                 [&wanted](const AudioEncode& e) { return e.id == wanted; });
    return it == audio_encodes.end() ? nullptr : &*it;
}

const SceneLink* Project::scene_link(const Id& wanted) const {
    const auto it = std::find_if(scene_links.begin(), scene_links.end(),
                                 [&wanted](const SceneLink& l) { return l.id == wanted; });
    return it == scene_links.end() ? nullptr : &*it;
}

const Bus* Project::bus(const Id& wanted) const {
    const auto it =
        std::find_if(buses.begin(), buses.end(), [&wanted](const Bus& b) { return b.id == wanted; });
    return it == buses.end() ? nullptr : &*it;
}

const Source* Project::source(const Id& wanted) const {
    const auto it = std::find_if(sources.begin(), sources.end(),
                                 [&wanted](const Source& s) { return s.id == wanted; });
    return it == sources.end() ? nullptr : &*it;
}

const std::vector<Effect>* Project::effect_stack(const Id& owner) const {
    if (const auto* value = source(owner))
        return &value->effects;
    if (const auto* value = bus(owner))
        return &value->inserts;
    for (const auto& value : scenes) {
        if (value.id == owner)
            return &value.effects;
        if (const auto* node = find_node(value.roots, owner))
            return &node->effects;
    }
    return nullptr;
}

std::vector<Effect>* Project::effect_stack(const Id& owner) {
    return const_cast<std::vector<Effect>*>(std::as_const(*this).effect_stack(owner));
}

bool is_self_contained(std::string_view source_type) {
    return std::find(kSelfContainedSources.begin(), kSelfContainedSources.end(), source_type) !=
           kSelfContainedSources.end();
}

bool has_audio(std::string_view source_type) {
    return std::find(kSilentSources.begin(), kSilentSources.end(), source_type) ==
           kSilentSources.end();
}

bool has_audio(const Source& source) {
    if (source.type == "source.clone") {
        const auto it = source.settings.find("audio");
        const auto* enabled = it == source.settings.end() ? nullptr : std::get_if<bool>(&it->second);
        return enabled && *enabled;
    }
    if (source.type == "source.media" || source.type == "source.playlist" ||
        source.type == "source.browser") {
        const auto it = source.settings.find("ignoreAudio");
        if (it != source.settings.end())
            if (const auto* ignored = std::get_if<bool>(&it->second); ignored && *ignored)
                return false;
    }
    if (source.type != "source.v4l2")
        return has_audio(source.type);
    const auto it = source.settings.find("audioDevice");
    const auto* device =
        it == source.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    return device && !device->empty() && *device != "none";
}

std::optional<Source> resolve_source(const Project& project, const Source& source, bool audio) {
    const Source* input = &source;
    std::vector<const Source*> clones;
    while (input->type == "source.clone") {
        // ponytail: clone chains stop at 64; use a dependency graph for deeper compositions.
        if ((audio && !has_audio(*input)) || clones.size() >= 64 ||
            clones.size() >= project.sources.size() ||
            std::ranges::find(clones, input) != clones.end())
            return std::nullopt;
        clones.push_back(input);
        const auto it = input->settings.find("target");
        const auto* text = it == input->settings.end() ? nullptr : std::get_if<std::string>(&it->second);
        const auto id = text ? Id::parse(*text) : std::nullopt;
        input = id ? project.source(*id) : nullptr;
        if (!input)
            return std::nullopt;
    }
    Source resolved = *input;
    for (auto it = clones.rbegin(); it != clones.rend(); ++it) {
        const auto setting = (*it)->settings.find("noFilters");
        if (!audio && setting != (*it)->settings.end())
            if (const auto* bypass = std::get_if<bool>(&setting->second); bypass && *bypass)
                resolved.effects.clear();
        resolved.effects.insert(resolved.effects.end(), (*it)->effects.begin(), (*it)->effects.end());
    }
    return resolved;
}

std::vector<Problem> check_emergency_scene(const Project& project, const Id& scene_id) {
    std::vector<Problem> out;
    const Scene* scene = project.scene(scene_id);
    if (!scene) {
        out.push_back(Problem{Severity::Warning,
                              std::format("the emergency scene {} does not exist, so this canvas "
                                          "would fall back to black and silence",
                                          scene_id.to_string()),
                              scene_id});
        return out;
    }

    std::size_t media_nodes = 0;
    std::size_t total_nodes = 0;
    visit(scene->roots, [&](const Node& n) {
        const auto* placed = std::get_if<SourceData>(&n.data);
        if (!placed)
            return;
        ++total_nodes;
        if (!placed->source) {
            // A source node bound to nothing draws nothing, which makes the safety net exactly as
            // useful as not having one -- while looking, in the scene list, as though it is set.
            out.push_back(Problem{Severity::Warning,
                                  std::format("'{}' in the emergency scene is not bound to "
                                              "anything, so it would show nothing",
                                              n.name),
                                  n.id});
            return;
        }
        const Source* src = project.source(*placed->source);
        if (!src) {
            out.push_back(Problem{Severity::Warning,
                                  std::format("'{}' in the emergency scene has no source", n.name),
                                  n.id});
            return;
        }
        if (!is_self_contained(src->type)) {
            out.push_back(
                Problem{Severity::Warning,
                        std::format("the emergency scene uses '{}', which is a {} -- it can fail "
                                    "at exactly the moment the emergency scene is needed",
                                    src->name, src->type),
                        n.id});
            return;
        }
        if (src->type != "source.media")
            return;
        ++media_nodes;

        // The safe state can last minutes. A clip that plays once and cuts to black is a net
        // that expires, usually just as it is finally doing its job.
        const auto loop = src->settings.find("loop");
        const bool loops = loop != src->settings.end() &&
                           std::holds_alternative<bool>(loop->second) &&
                           std::get<bool>(loop->second);
        if (!loops)
            out.push_back(Problem{Severity::Warning,
                                  std::format("'{}' is not set to loop, so the emergency scene "
                                              "would cut to black when the clip ends",
                                              src->name),
                                  n.id});
    });

    if (total_nodes == 0) {
        out.push_back(Problem{Severity::Warning,
                              std::format("the emergency scene '{}' has nothing in it",
                                          scene->name),
                              scene->id});
    } else if (media_nodes > 1 || total_nodes > 1) {
        // Not an error: a logo over a clip is reasonable. But the net is reliable in proportion
        // to how little it depends on, and one pre-rendered clip can carry any branding wanted.
        out.push_back(Problem{Severity::Warning,
                              std::format("the emergency scene '{}' has {} sources; one looping "
                                          "clip is the most reliable shape for it",
                                          scene->name, total_nodes),
                              scene->id});
    }
    return out;
}

Project make_default_project(std::string name) {
    Project p;
    p.id = Id::generate();
    p.name = std::move(name);

    Canvas canvas;
    canvas.id = Id::generate();
    canvas.name = "Main";
    canvas.width = 1920;
    canvas.height = 1080;
    canvas.fps = Fps{60, 1};
    p.canvases.push_back(canvas);

    Bus program;
    program.id = Id::generate();
    program.name = "Program";
    program.kind = BusKind::Program;
    p.buses.push_back(program);

    // The copyright-safe recording track, present from the start because adding it later means
    // re-routing every source that already exists.
    Bus vod;
    vod.id = Id::generate();
    vod.name = "VOD";
    vod.kind = BusKind::Vod;
    p.buses.push_back(vod);

    Scene scene;
    scene.id = Id::generate();
    scene.name = "Scene";
    scene.canvas = canvas.id;
    p.scenes.push_back(scene);

    Deck deck;
    deck.id = Id::generate();
    deck.name = "Main";
    deck.canvas = canvas.id;
    deck.program_scene = scene.id;
    p.decks.push_back(deck);

    return p;
}

Scene duplicate_scene(const Scene& scene, std::string new_name) {
    Scene copy;
    copy.id = Id::generate();
    copy.name = std::move(new_name);
    // The copy is authored for the same canvas. A duplicate that landed somewhere else would be
    // the wrong size, and nothing in the gesture says which other canvas was meant.
    copy.canvas = scene.canvas;
    std::map<Id, Id> remap;
    copy.roots = duplicate(scene.roots, &remap);
    copy.effects = scene.effects;
    for (auto& effect : copy.effects) {
        effect.id = Id::generate();
        if (effect.mask && remap.contains(*effect.mask))
            effect.mask = remap.at(*effect.mask);
    }
    // Audio overrides are keyed by node id, so they have to follow the remap or they would
    // silently apply to nothing.
    for (const auto& [node_id, ov] : scene.audio_overrides) {
        const auto it = remap.find(node_id);
        copy.audio_overrides.emplace(it == remap.end() ? node_id : it->second, ov);
    }
    return copy;
}

// ---- validation -----------------------------------------------------------------------------

namespace {

class Validator {
public:
    explicit Validator(const Project& p) : p_(p) {}

    std::vector<Problem> run() {
        index_ids();
        check_schema();
        check_canvases();
        check_buses();
        check_sources_and_assets();
        check_scenes();
        check_scene_links();
        check_decks();
        check_encodes();
        check_audio_encodes();
        check_outputs();
        check_reference_cycles();
        check_precomp_cycles();
        return std::move(problems_);
    }

private:
    void err(const Id& subject, std::string msg) {
        problems_.push_back(Problem{Severity::Error, std::move(msg), subject});
    }
    void warn(const Id& subject, std::string msg) {
        problems_.push_back(Problem{Severity::Warning, std::move(msg), subject});
    }

    void define(const Id& id, std::string_view kind, std::string name) {
        if (id.is_nil()) {
            err(id, std::format("a {} named '{}' has no id", kind, name));
            return;
        }
        const auto [it, fresh] = owners_.emplace(id, Owner{kind, std::move(name)});
        if (!fresh) {
            err(id, std::format("id {} is used by both the {} '{}' and a {}", id.to_string(),
                                it->second.kind, it->second.name, kind));
        }
    }

    void index_ids() {
        define(p_.id, "project", p_.name);
        for (const Canvas& c : p_.canvases)
            define(c.id, "canvas", c.name);
        for (const Bus& b : p_.buses) {
            define(b.id, "bus", b.name);
            for (const Effect& e : b.inserts)
                define(e.id, "insert", e.type);
        }
        for (const Source& s : p_.sources) {
            define(s.id, "source", s.name);
            for (const auto& effect : s.effects)
                define(effect.id, "effect", effect.type);
        }
        for (const Asset& a : p_.assets)
            define(a.id, "asset", a.path);
        for (const Deck& d : p_.decks)
            define(d.id, "deck", d.name);
        for (const Encode& e : p_.encodes)
            define(e.id, "encode", e.name);
        for (const AudioEncode& e : p_.audio_encodes)
            define(e.id, "audio encode", e.name);
        for (const Output& o : p_.outputs)
            define(o.id, "output", o.name);
        for (const SceneLink& l : p_.scene_links)
            define(l.id, "scene link", "");
        for (const Scene& s : p_.scenes) {
            define(s.id, "scene", s.name);
            for (const auto& effect : s.effects)
                define(effect.id, "effect", effect.type);
            visit(s.roots, [&](const Node& n) {
                define(n.id, to_string(n.kind()), n.name);
                for (const Effect& e : n.effects)
                    define(e.id, "effect", e.type);
            });
        }
    }

    [[nodiscard]] bool exists_as(const Id& id, std::string_view kind) const {
        const auto it = owners_.find(id);
        return it != owners_.end() && it->second.kind == kind;
    }

    void check_schema() {
        if (p_.schema_version <= 0)
            err(p_.id, std::format("schema_version is {}", p_.schema_version));
        if (p_.schema_version > kSchemaVersion)
            err(p_.id, std::format("schema_version {} is newer than this build understands ({})",
                                   p_.schema_version, kSchemaVersion));
        if (p_.settings.sample_rate <= 0)
            err(p_.id, std::format("sample rate is {}", p_.settings.sample_rate));
        if (p_.canvases.empty())
            warn(p_.id, "the project has no canvases, so nothing can be rendered");
    }

    void check_canvases() {
        for (const Canvas& c : p_.canvases) {
            if (c.width <= 0 || c.height <= 0)
                err(c.id, std::format("canvas '{}' is {}x{}", c.name, c.width, c.height));
            if (!c.fps.valid())
                err(c.id, std::format("canvas '{}' has frame rate {}/{}", c.name, c.fps.num,
                                      c.fps.den));
            // An emergency scene that is set but unsuitable is a property of the document, so
            // it belongs here. One that is simply *absent* is not: a project at rest is not
            // wrong for lacking a safety net, and warning about it would mean every new project
            // is born complaining. "Does every canvas have one?" is a pre-flight question, asked
            // when the operator is about to go live and it actually matters (ADR-0025).
            if (c.emergency_scene) {
                for (Problem& problem : check_emergency_scene(p_, *c.emergency_scene))
                    problems_.push_back(std::move(problem));
                // New with ADR-0027, and a consequence of this field's own purpose: it is per
                // canvas so that a vertical show gets a separately authored vertical clip. A
                // net belonging to another canvas is the letterboxed landscape that reasoning
                // rejects. A warning rather than an error -- it does render, just wrongly.
                const Scene* net = p_.scene(*c.emergency_scene);
                if (net && !net->canvas.is_nil() && net->canvas != c.id)
                    warn(c.id, std::format("canvas '{}' falls back to scene '{}', which belongs "
                                           "to another canvas and will not be the right shape",
                                           c.name, net->name));
            }
        }
    }

    void check_buses() {
        for (const Bus& b : p_.buses) {
            if (b.channels <= 0)
                err(b.id, std::format("bus '{}' has {} channels", b.name, b.channels));
            for (const Send& s : b.sends) {
                if (s.to == b.id)
                    err(b.id, std::format("bus '{}' sends to itself", b.name));
                else if (!exists_as(s.to, "bus"))
                    err(b.id, std::format("bus '{}' sends to {}, which is not a bus", b.name,
                                          s.to.to_string()));
            }
            for (const Effect& e : b.inserts) {
                if (e.type.empty())
                    err(e.id, std::format("an insert on bus '{}' has no type", b.name));
            }
        }
    }

    void check_sources_and_assets() {
        for (const Source& s : p_.sources) {
            if (s.effects.size() > 64)
                err(s.id, "source filter stack exceeds 64 filters");
            for (const auto& effect : s.effects)
                if (effect.type.empty())
                    err(effect.id, "source filter has no type");
            if (s.type.empty())
                err(s.id, std::format("source '{}' has no type", s.name));
            if (s.type == "source.clone" && !resolve_source(p_, s))
                warn(s.id, std::format("source clone '{}' has a missing input or a clone cycle", s.name));
            if (s.asset && !exists_as(*s.asset, "asset"))
                warn(s.id, std::format("source '{}' refers to asset {}, which is not in the "
                                       "manifest",
                                       s.name, s.asset->to_string()));
        }
        for (const Asset& a : p_.assets) {
            if (a.path.empty())
                err(a.id, "an asset has no path");
        }
    }

    void check_scenes() {
        for (const Scene& s : p_.scenes) {
            if (s.effects.size() > 64)
                err(s.id, "scene filter stack exceeds 64 filters");
            for (const auto& effect : s.effects)
                if (effect.type.empty())
                    err(effect.id, "scene filter has no type");
            // Not a dangling reference but a missing precondition, for the same reason a deck's
            // canvas is: a scene with no canvas has no size and no frame rate, so there is no
            // way to render it at all (ADR-0027).
            if (!exists_as(s.canvas, "canvas"))
                err(s.id, std::format("scene '{}' belongs to canvas {}, which does not exist",
                                      s.name, s.canvas.to_string()));

            std::unordered_set<Id> in_scene;
            std::unordered_set<Id> ignored_audio;
            visit(s.roots, [&](const Node& n) {
                in_scene.insert(n.id);
                if (const auto* placed = std::get_if<SourceData>(&n.data)) {
                    const auto* source = placed->source ? p_.source(*placed->source) : nullptr;
                    if (source && !has_audio(*source) &&
                        (source->type == "source.media" || source->type == "source.playlist" ||
                         source->type == "source.browser"))
                        ignored_audio.insert(n.id);
                }
                if (n.blend.opacity < 0.0 || n.blend.opacity > 1.0)
                    err(n.id, std::format("'{}' has opacity {}", n.name, n.blend.opacity));
                for (const Effect& e : n.effects) {
                    if (e.type.empty())
                        err(e.id, std::format("an effect on '{}' has no type", n.name));
                }
                if (const auto* g = std::get_if<GroupData>(&n.data)) {
                    if (g->size && (!std::isfinite(g->size->x) || !std::isfinite(g->size->y) ||
                                    g->size->x <= 0 || g->size->y <= 0 || g->size->x > 8192 ||
                                    g->size->y > 8192 || g->size->x * g->size->y > 33554432))
                        err(n.id, "group surface bounds exceed supported range");
                } else if (const auto* ref = std::get_if<ReferenceData>(&n.data)) {
                    if (ref->target == n.id)
                        err(n.id, std::format("'{}' references itself", n.name));
                }
            });

            std::vector<Reference> refs;
            collect_references(s.roots, refs);
            for (const Reference& r : refs) {
                // Disabled audio keeps its saved routing for re-enabling, without warnings.
                if (r.kind == RefKind::Bus && ignored_audio.contains(r.from))
                    continue;
                const char* kind = nullptr;
                switch (r.kind) {
                case RefKind::Source: kind = "source"; break;
                case RefKind::Scene:  kind = "scene"; break;
                case RefKind::Bus:    kind = "bus"; break;
                case RefKind::Node:   kind = nullptr; break;
                }
                // A dangling reference is a *warning*, deliberately.
                //
                // It is not structurally broken: the node draws nothing, exactly as it would
                // with no reference set at all, and the only difference is that this one records
                // that something was deleted. Making it an error would mean a scene could not be
                // deleted while anything cloned it and a source could not be removed while any
                // source node used it -- a web of refusals that makes ordinary editing a puzzle.
                // The operator is told what is missing and can fix it; nothing silently renders
                // wrong. Errors are reserved for documents that cannot be rendered as written.
                if (kind) {
                    if (!exists_as(r.id, kind)) {
                        const auto* node = find_node(s.roots, r.from);
                        warn(r.from, std::format("scene '{}', source '{}': refers to {} {}, "
                                                 "which does not exist",
                                                 s.name, node ? node->name : r.from.to_string(),
                                                 ref_kind_name(r.kind), r.id.to_string()));
                    }
                } else {
                    // Node references resolve within the project, not only within this scene: a
                    // Reference may legitimately point at a node in another scene.
                    if (owners_.find(r.id) == owners_.end())
                        warn(r.from,
                             std::format("refers to node {}, which does not exist",
                                         r.id.to_string()));
                }
            }

            for (const auto& [node_id, ov] : s.audio_overrides) {
                if (!in_scene.contains(node_id))
                    warn(s.id, std::format("scene '{}' has audio routing for node {}, which is "
                                           "not in it",
                                           s.name, node_id.to_string()));
                if (ov.bus && !ignored_audio.contains(node_id) && !exists_as(*ov.bus, "bus"))
                    warn(s.id, std::format("scene '{}' routes a source to bus {}, which does not "
                                           "exist",
                                           s.name, ov.bus->to_string()));
            }
        }
    }

    /// The three rules from ADR-0027 that a stored link must satisfy, checked here as well as in
    /// the command, because a hand-edited or migrated project never went through a command.
    void check_scene_links() {
        std::set<std::pair<Id, Id>> seen; // (from, the canvas it cues)
        for (const SceneLink& l : p_.scene_links) {
            const Scene* from = p_.scene(l.from);
            const Scene* to = p_.scene(l.to);
            // A link naming a scene that is gone cues nothing, exactly like a deck pointing at
            // a deleted scene. Warning, not error, so a scene stays deletable (ADR-0023).
            if (!from) {
                warn(l.id, std::format("a scene link starts at scene {}, which does not exist",
                                       l.from.to_string()));
                continue;
            }
            if (!to) {
                warn(l.id, std::format("the link from scene '{}' cues scene {}, which does not "
                                       "exist",
                                       from->name, l.to.to_string()));
                continue;
            }
            if (from->canvas == to->canvas) {
                err(l.id, std::format("the link from '{}' cues '{}' on the same canvas, which "
                                      "would be a scene activating itself",
                                      from->name, to->name));
                continue;
            }
            // Links run from Main and only from Main. A warning rather than an error: such a
            // link cannot be made through the shell any more, but a project written by hand or
            // by an earlier build still opens and still renders -- the link simply never fires,
            // because only Main is watched for cues.
            if (!p_.canvases.empty() && from->canvas != p_.canvases.front().id) {
                warn(l.id, std::format("the link to '{}' starts at '{}', which is not on '{}'; "
                                       "only scenes on the first canvas cue anything, so this "
                                       "link will never fire",
                                       to->name, from->name, p_.canvases.front().name));
            }
            if (!seen.emplace(l.from, to->canvas).second)
                err(l.id, std::format("scene '{}' cues more than one scene on the canvas '{}' "
                                      "holds, so there is no answer to which one",
                                      from->name, to->name));
            const Canvas* target = p_.canvas(to->canvas);
            if (target && target->emergency_scene && *target->emergency_scene == l.to)
                err(l.id, std::format("the link from '{}' cues '{}', which is that canvas's "
                                      "emergency scene; a safety net something else can fire is "
                                      "not one",
                                      from->name, to->name));
        }
    }

    void check_decks() {
        for (const Deck& d : p_.decks) {
            if (!exists_as(d.canvas, "canvas"))
                err(d.id, std::format("deck '{}' targets canvas {}, which does not exist", d.name,
                                      d.canvas.to_string()));
            // Same reasoning as a dangling reference: a deck pointing at a deleted scene shows
            // nothing, which is what a deck with no program scene does anyway. The canvas above
            // is different -- a deck without one has no output surface at all.
            if (d.program_scene && !exists_as(*d.program_scene, "scene"))
                warn(d.id, std::format("deck '{}' has a program scene that was removed", d.name));
            if (d.preview_scene && !exists_as(*d.preview_scene, "scene"))
                warn(d.id, std::format("deck '{}' has a preview scene that was removed", d.name));
            if (d.transition_ms < 0)
                err(d.id, std::format("deck '{}' has a transition of {} ms", d.name,
                                      d.transition_ms));
        }
    }

    void check_encodes() {
        for (const Encode& e : p_.encodes) {
            if (e.canvas.is_nil())
                warn(e.id, std::format("encode '{}' has no canvas connected", e.name));
            else if (!exists_as(e.canvas, "canvas"))
                err(e.id, std::format("encode '{}' encodes canvas {}, which does not exist",
                                      e.name, e.canvas.to_string()));
            if (e.encoder.bitrate_kbps <= 0)
                err(e.id, std::format("encode '{}' has a bitrate of {} kbps", e.name,
                                      e.encoder.bitrate_kbps));
            if (e.rescale && (e.rescale->x <= 0.0 || e.rescale->y <= 0.0))
                err(e.id, std::format("encode '{}' rescales to {}x{}", e.name, e.rescale->x,
                                      e.rescale->y));
        }
    }

    void check_audio_encodes() {
        for (const AudioEncode& e : p_.audio_encodes) {
            if (e.bus.is_nil())
                warn(e.id, std::format("audio encode '{}' has no bus connected", e.name));
            else if (!exists_as(e.bus, "bus"))
                err(e.id, std::format("audio encode '{}' encodes bus {}, which does not exist",
                                      e.name, e.bus.to_string()));
            if (e.bitrate_kbps <= 0)
                err(e.id, std::format("audio encode '{}' has a bitrate of {} kbps", e.name,
                                      e.bitrate_kbps));
        }
    }

    void check_outputs() {
        for (const Output& o : p_.outputs) {
            // Nil is an explicitly disconnected port. A non-nil dangling id is still an error.
            if (o.encode.is_nil())
                warn(o.id, std::format("output '{}' has no video encode connected", o.name));
            else if (!exists_as(o.encode, "encode"))
                err(o.id, std::format("output '{}' sends encode {}, which does not exist",
                                      o.name, o.encode.to_string()));
            // Carrying no audio is a choice -- a silent file capture means it -- so this is a
            // warning. Naming one that is not there is not a choice.
            if (o.audio_encode && !exists_as(*o.audio_encode, "audio encode"))
                err(o.id, std::format("output '{}' sends audio encode {}, which does not exist",
                                      o.name, o.audio_encode->to_string()));
            else if (!o.audio_encode && o.enabled)
                warn(o.id, std::format("output '{}' carries no audio", o.name));
            if (o.enabled && o.destination.url.empty() && o.destination.kind != DestinationKind::Replay)
                err(o.id, std::format("output '{}' is enabled but has no destination", o.name));
        }
    }

    /// A Reference pointing, directly or through other References, back at itself would make the
    /// renderer recurse until it died. Catching it here means the renderer never has to carry a
    /// depth limit whose only purpose is to survive a document it should have refused.
    void check_reference_cycles() {
        std::unordered_map<Id, Id> edges;
        for (const Scene& s : p_.scenes) {
            visit(s.roots, [&](const Node& n) {
                if (const auto* ref = std::get_if<ReferenceData>(&n.data))
                    edges.emplace(n.id, ref->target);
            });
        }
        for (const auto& [from, to] : edges) {
            (void)to;
            std::unordered_set<Id> seen{from};
            Id at = from;
            for (;;) {
                const auto it = edges.find(at);
                if (it == edges.end())
                    break;
                at = it->second;
                if (!seen.insert(at).second) {
                    err(from, std::format("reference {} is part of a loop", from.to_string()));
                    break;
                }
            }
        }
    }

    /// The same problem one level up: scene A precomps scene B precomps scene A.
    void check_precomp_cycles() {
        std::unordered_map<Id, std::vector<Id>> edges;
        for (const Scene& s : p_.scenes) {
            visit(s.roots, [&](const Node& n) {
                if (const auto* pre = std::get_if<PrecompData>(&n.data))
                    edges[s.id].push_back(pre->scene);
            });
        }

        std::unordered_set<Id> done;
        std::unordered_set<Id> on_stack;
        const std::function<bool(const Id&)> walk = [&](const Id& scene_id) {
            if (on_stack.contains(scene_id))
                return true;
            if (done.contains(scene_id))
                return false;
            on_stack.insert(scene_id);
            bool cycle = false;
            const auto it = edges.find(scene_id);
            if (it != edges.end()) {
                for (const Id& next : it->second)
                    cycle = walk(next) || cycle;
            }
            on_stack.erase(scene_id);
            done.insert(scene_id);
            return cycle;
        };
        for (const Scene& s : p_.scenes) {
            if (walk(s.id))
                err(s.id, std::format("scene '{}' precomposes itself, directly or through "
                                      "another scene",
                                      s.name));
        }
    }

    const Project& p_;
    std::unordered_map<Id, Owner> owners_;
    std::vector<Problem> problems_;
};

} // namespace

std::vector<Problem> validate(const Project& project) {
    return Validator{project}.run();
}

bool is_valid(const Project& project) {
    const auto problems = validate(project);
    return std::none_of(problems.begin(), problems.end(),
                        [](const Problem& p) { return p.severity == Severity::Error; });
}

std::string default_encode_name(const Project& project, const Encode& encode) {
    const Canvas* c = project.canvas(encode.canvas);
    const double mbps = static_cast<double>(encode.encoder.bitrate_kbps) / 1000.0;
    const std::string rate = mbps >= 10.0 || mbps == static_cast<double>(static_cast<int>(mbps))
                                 ? std::format("{:.0f}", mbps)
                                 : std::format("{:.1f}", mbps);
    if (!c)
        return std::format("{} Mbps", rate);
    const double fps =
        c->fps.den == 0 ? 0.0
                        : static_cast<double>(c->fps.num) / static_cast<double>(c->fps.den);
    // The height it *sends*, not the canvas's. An encode that rescales to 1080 off a 1440
    // canvas is a 1080p encode, and calling it 1440p is the same mistake the outputs row made.
    const std::int32_t height =
        encode.rescale ? static_cast<std::int32_t>(encode.rescale->y) : c->height;
    return std::format("{}p{:.0f} {} Mbps", height, fps, rate);
}

} // namespace ldoc
