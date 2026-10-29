// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/command.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <unordered_set>

namespace ldoc {
namespace {

/// Why adding `node` to `scene` would make a precomposition loop, or nothing when it would not.
///
/// Walks the scenes a precomp would reach, following precomps already in the project, and
/// reports the first path that arrives back at `scene`. Returned as a message rather than a
/// bool so the refusal can name the scene the operator has to break the loop at.
std::optional<std::string> precomp_cycle_reason(const Project& p, const Id& scene,
                                                const Node& node) {
    std::vector<Id> reaches;
    visit(node, [&](const Node& n) {
        if (const auto* pre = std::get_if<PrecompData>(&n.data))
            reaches.push_back(pre->scene);
    });
    if (reaches.empty())
        return std::nullopt;

    std::unordered_set<Id> seen;
    const std::function<bool(const Id&)> hits_scene = [&](const Id& at) {
        if (at == scene)
            return true;
        if (!seen.insert(at).second)
            return false;
        const Scene* s = p.scene(at);
        if (!s)
            return false;
        bool found = false;
        visit(s->roots, [&](const Node& n) {
            if (found)
                return;
            if (const auto* pre = std::get_if<PrecompData>(&n.data))
                found = hits_scene(pre->scene);
        });
        return found;
    };

    const Scene* into = p.scene(scene);
    for (const Id& target : reaches) {
        if (!hits_scene(target))
            continue;
        const Scene* t = p.scene(target);
        return std::format(
            "'{}' already shows '{}', directly or through another scene, so adding it here "
            "would make a loop",
            t ? t->name : target.to_string(), into ? into->name : scene.to_string());
    }
    return std::nullopt;
}


/// A node together with the list it lives in, so it can be removed, reinserted or moved.
struct Located {
    std::vector<Node>* siblings = nullptr;
    std::size_t index = 0;
    std::optional<Id> parent;
};

std::optional<Located> locate(std::vector<Node>& roots, const Id& wanted,
                              std::optional<Id> parent = std::nullopt) {
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].id == wanted)
            return Located{&roots, i, parent};
        if (auto* kids = roots[i].children()) {
            if (auto found = locate(*kids, wanted, roots[i].id))
                return found;
        }
    }
    return std::nullopt;
}

std::expected<Scene*, std::string> find_scene(Project& p, const Id& id) {
    Scene* s = p.scene(id);
    if (!s)
        return std::unexpected(std::format("there is no scene {}", id.to_string()));
    return s;
}

std::expected<Node*, std::string> find_in_scene(Scene& s, const Id& id) {
    Node* n = find_node(s.roots, id);
    if (!n)
        return std::unexpected(
            std::format("there is no node {} in scene '{}'", id.to_string(), s.name));
    return n;
}

std::expected<Effect*, std::string> find_effect(Node& n, const Id& id) {
    const auto it = std::find_if(n.effects.begin(), n.effects.end(),
                                 [&id](const Effect& e) { return e.id == id; });
    if (it == n.effects.end())
        return std::unexpected(
            std::format("there is no effect {} on '{}'", id.to_string(), n.name));
    return &*it;
}

std::expected<Deck*, std::string> find_deck(Project& p, const Id& id) {
    const auto it = std::find_if(p.decks.begin(), p.decks.end(),
                                 [&id](const Deck& d) { return d.id == id; });
    if (it == p.decks.end())
        return std::unexpected(std::format("there is no deck {}", id.to_string()));
    return &*it;
}

/// Every id the project already defines. Used to refuse an insert that would create a duplicate,
/// which is a corruption the validator would catch later but the command should never create.
std::unordered_set<Id> all_ids(const Project& p) {
    std::unordered_set<Id> ids;
    ids.insert(p.id);
    for (const Canvas& c : p.canvases)
        ids.insert(c.id);
    for (const Bus& b : p.buses) {
        ids.insert(b.id);
        for (const Effect& e : b.inserts)
            ids.insert(e.id);
    }
    for (const Source& s : p.sources) {
        ids.insert(s.id);
        for (const auto& effect : s.effects)
            ids.insert(effect.id);
    }
    for (const Asset& a : p.assets)
        ids.insert(a.id);
    for (const Deck& d : p.decks)
        ids.insert(d.id);
    for (const Encode& e : p.encodes)
        ids.insert(e.id);
    for (const AudioEncode& e : p.audio_encodes)
        ids.insert(e.id);
    for (const Output& o : p.outputs)
        ids.insert(o.id);
    for (const SceneLink& l : p.scene_links)
        ids.insert(l.id);
    for (const Scene& s : p.scenes) {
        ids.insert(s.id);
        for (const auto& effect : s.effects)
            ids.insert(effect.id);
        visit(s.roots, [&ids](const Node& n) {
            ids.insert(n.id);
            for (const Effect& e : n.effects)
                ids.insert(e.id);
        });
    }
    return ids;
}

/// True when `candidate` is `root` or lives beneath it.
bool is_within(const Node& root, const Id& candidate) {
    bool found = false;
    visit(root, [&](const Node& n) { found = found || n.id == candidate; });
    return found;
}

std::string quoted(const std::string& s) {
    return s.empty() ? std::string{"(unnamed)"} : std::format("'{}'", s);
}

const char* node_label(const Project& p, const Id& scene, const Id& node) {
    const Scene* s = p.scene(scene);
    if (!s)
        return "node";
    const Node* n = find_node(s->roots, node);
    return (n && !n->name.empty()) ? n->name.c_str() : "node";
}

} // namespace

std::string_view command_name(const Command& command) {
    return std::visit(
        [](const auto& c) -> std::string_view {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetProjectName>) return "project.set-name";
            else if constexpr (std::is_same_v<T, AddScene>) return "scene.add";
            else if constexpr (std::is_same_v<T, RemoveScene>) return "scene.remove";
            else if constexpr (std::is_same_v<T, RenameScene>) return "scene.rename";
            else if constexpr (std::is_same_v<T, MoveScene>) return "scene.move";
            else if constexpr (std::is_same_v<T, AddNode>) return "node.add";
            else if constexpr (std::is_same_v<T, RemoveNode>) return "node.remove";
            else if constexpr (std::is_same_v<T, MoveNode>) return "node.move";
            else if constexpr (std::is_same_v<T, RenameNode>) return "node.rename";
            else if constexpr (std::is_same_v<T, SetNodeVisible>) return "node.set-visible";
            else if constexpr (std::is_same_v<T, SetNodeLocked>) return "node.set-locked";
            else if constexpr (std::is_same_v<T, SetNodeTransform>) return "node.set-transform";
            else if constexpr (std::is_same_v<T, SetNodeLayout>) return "node.set-layout";
            else if constexpr (std::is_same_v<T, SetNodeBlend>) return "node.set-blend";
            else if constexpr (std::is_same_v<T, SetNodeAudio>) return "node.set-audio";
            else if constexpr (std::is_same_v<T, AddDeck>) return "deck.add";
            else if constexpr (std::is_same_v<T, RemoveDeck>) return "deck.remove";
            else if constexpr (std::is_same_v<T, SetDeck>) return "deck.set";
            else if constexpr (std::is_same_v<T, SetSceneFavourite>) return "scene.set-favourite";
            else if constexpr (std::is_same_v<T, Batch>) return "batch";
            else if constexpr (std::is_same_v<T, AddEffect>) return "effect.add";
            else if constexpr (std::is_same_v<T, SetEffects>)
                return "effects.set";
            else if constexpr (std::is_same_v<T, RemoveEffect>) return "effect.remove";
            else if constexpr (std::is_same_v<T, SetEffectParams>) return "effect.set-params";
            else if constexpr (std::is_same_v<T, SetEffectEnabled>) return "effect.set-enabled";
            else if constexpr (std::is_same_v<T, SetDeckProgram>) return "deck.set-program";
            else if constexpr (std::is_same_v<T, SetDeckPreview>) return "deck.set-preview";
            else if constexpr (std::is_same_v<T, SetCanvasEmergencyScene>)
                return "canvas.set-emergency-scene";
            else if constexpr (std::is_same_v<T, SetNodeSource>) return "node.set-source";
            else if constexpr (std::is_same_v<T, AddSource>) return "source.add";
            else if constexpr (std::is_same_v<T, RemoveSource>) return "source.remove";
            else if constexpr (std::is_same_v<T, SetSource>) return "source.set";
            else if constexpr (std::is_same_v<T, AddSceneLink>) return "scene-link.add";
            else if constexpr (std::is_same_v<T, RemoveSceneLink>) return "scene-link.remove";
            else if constexpr (std::is_same_v<T, AddCanvas>) return "canvas.add";
            else if constexpr (std::is_same_v<T, RemoveCanvas>) return "canvas.remove";
            else if constexpr (std::is_same_v<T, SetCanvas>) return "canvas.set";
            else if constexpr (std::is_same_v<T, AddBus>) return "bus.add";
            else if constexpr (std::is_same_v<T, RemoveBus>) return "bus.remove";
            else if constexpr (std::is_same_v<T, SetBus>) return "bus.set";
            else if constexpr (std::is_same_v<T, AddEncode>) return "encode.add";
            else if constexpr (std::is_same_v<T, RemoveEncode>) return "encode.remove";
            else if constexpr (std::is_same_v<T, SetEncode>) return "encode.set";
            else if constexpr (std::is_same_v<T, AddAudioEncode>) return "audio-encode.add";
            else if constexpr (std::is_same_v<T, RemoveAudioEncode>) return "audio-encode.remove";
            else if constexpr (std::is_same_v<T, SetAudioEncode>) return "audio-encode.set";
            else if constexpr (std::is_same_v<T, AddOutput>) return "output.add";
            else if constexpr (std::is_same_v<T, RemoveOutput>) return "output.remove";
            else if constexpr (std::is_same_v<T, SetOutput>) return "output.set";
            else return "project.set-settings";
        },
        command);
}

std::string describe(const Command& command) {
    return std::visit(
        [](const auto& c) -> std::string {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetProjectName>)
                return std::format("Rename project to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, AddScene>)
                return std::format("Add scene {}", quoted(c.scene.name));
            else if constexpr (std::is_same_v<T, RemoveScene>)
                return "Remove scene";
            else if constexpr (std::is_same_v<T, RenameScene>)
                return std::format("Rename scene to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, MoveScene>)
                return "Reorder scene";
            else if constexpr (std::is_same_v<T, AddNode>)
                return std::format("Add {}", quoted(c.node.name));
            else if constexpr (std::is_same_v<T, RemoveNode>)
                return "Remove from scene";
            else if constexpr (std::is_same_v<T, MoveNode>)
                return "Move source";
            else if constexpr (std::is_same_v<T, RenameNode>)
                return std::format("Rename source to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, SetNodeVisible>)
                return c.visible ? "Show source" : "Hide source";
            else if constexpr (std::is_same_v<T, SetNodeLocked>)
                return c.locked ? "Lock source" : "Unlock source";
            else if constexpr (std::is_same_v<T, SetNodeTransform>)
                return "Transform source";
            else if constexpr (std::is_same_v<T, SetNodeLayout>)
                return "Change source fit";
            else if constexpr (std::is_same_v<T, SetNodeBlend>)
                return "Change blending";
            else if constexpr (std::is_same_v<T, SetNodeAudio>)
                return c.audio.bus ? "Route source audio" : "Unroute source audio";
            else if constexpr (std::is_same_v<T, AddDeck>)
                return std::format("Add deck {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, RemoveDeck>)
                return "Remove deck";
            else if constexpr (std::is_same_v<T, SetDeck>)
                return std::format("Configure deck {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, SetSceneFavourite>)
                return c.favourite ? "Add to favourites" : "Remove from favourites";
            else if constexpr (std::is_same_v<T, Batch>)
                return c.description.empty() ? std::string{"Several changes"} : c.description;
            else if constexpr (std::is_same_v<T, AddEffect>)
                return std::format("Add effect {}", quoted(c.effect.type));
            else if constexpr (std::is_same_v<T, SetEffects>)
                return "Edit filters";
            else if constexpr (std::is_same_v<T, RemoveEffect>)
                return "Remove effect";
            else if constexpr (std::is_same_v<T, SetEffectParams>)
                return "Adjust effect";
            else if constexpr (std::is_same_v<T, SetEffectEnabled>)
                return c.enabled ? "Enable effect" : "Disable effect";
            else if constexpr (std::is_same_v<T, SetDeckProgram>)
                return c.scene ? "Switch program scene" : "Clear program scene";
            else if constexpr (std::is_same_v<T, SetDeckPreview>)
                return c.scene ? "Set preview scene" : "Clear preview scene";
            else if constexpr (std::is_same_v<T, SetCanvasEmergencyScene>)
                return c.scene ? "Set emergency scene" : "Clear emergency scene";
            else if constexpr (std::is_same_v<T, SetNodeSource>)
                return c.source ? "Bind to a source" : "Unbind source";
            else if constexpr (std::is_same_v<T, AddSource>)
                return std::format("Add source {}", quoted(c.source.name));
            else if constexpr (std::is_same_v<T, RemoveSource>)
                return "Remove source";
            else if constexpr (std::is_same_v<T, SetSource>)
                return std::format("Change source {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddSceneLink>)
                return "Link scenes";
            else if constexpr (std::is_same_v<T, RemoveSceneLink>)
                return "Unlink scenes";
            else if constexpr (std::is_same_v<T, AddCanvas>)
                return std::format("Add canvas {}", quoted(c.canvas.name));
            else if constexpr (std::is_same_v<T, RemoveCanvas>)
                return "Remove canvas";
            else if constexpr (std::is_same_v<T, SetCanvas>)
                return std::format("Change canvas {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddBus>)
                return std::format("Add bus {}", quoted(c.bus.name));
            else if constexpr (std::is_same_v<T, RemoveBus>)
                return "Remove bus";
            else if constexpr (std::is_same_v<T, SetBus>)
                return std::format("Change bus {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddEncode>)
                return std::format("Add encode {}", quoted(c.encode.name));
            else if constexpr (std::is_same_v<T, RemoveEncode>)
                return "Remove encode";
            else if constexpr (std::is_same_v<T, SetEncode>)
                return std::format("Change encode {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddAudioEncode>)
                return std::format("Add audio encode {}", quoted(c.encode.name));
            else if constexpr (std::is_same_v<T, RemoveAudioEncode>)
                return "Remove audio encode";
            else if constexpr (std::is_same_v<T, SetAudioEncode>)
                return std::format("Change audio encode {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddOutput>)
                return std::format("Add output {}", quoted(c.output.name));
            else if constexpr (std::is_same_v<T, RemoveOutput>)
                return "Remove output";
            else if constexpr (std::is_same_v<T, SetOutput>)
                return std::format("Change output {}", quoted(c.value.name));
            else
                return "Change project settings";
        },
        command);
}

namespace {

/// One overload per command. Each validates completely before touching the project, and returns
/// the command that puts things back.
/// A command validates its *preconditions* -- the things it acts on must exist -- but not its
/// *references*.
///
/// The distinction is forced by undo. Setting a deck's program scene captures the previous one
/// as its inverse; if that scene is later deleted, the inverse now points at something gone. Undo
/// has to be able to restore it anyway, because that is the state the command replaced. A command
/// may therefore only refuse states the document model itself refuses -- and a dangling reference
/// is a warning there, not an error, precisely so that deleting things stays possible.
///
/// Keeping the operator from *choosing* a scene that does not exist is the UI's job: it offers
/// the ones that do.
struct Apply {
    Project& p;

    std::expected<Applied, std::string> operator()(const Batch& c) const {
        if (!c.body || c.body->commands.empty())
            return std::unexpected(std::string{"a batch with nothing in it does nothing"});
        for (const Command& member : c.body->commands) {
            if (std::holds_alternative<Batch>(member))
                return std::unexpected(std::string{"a batch cannot contain another batch"});
        }

        // One entry per member, because a member's inverse may itself be several commands --
        // removing a scene has to put its links back too. Those stay in their own order while
        // the *members* are reversed, which a single flat list could not express.
        std::vector<std::vector<Command>> steps;
        steps.reserve(c.body->commands.size());
        const auto unwind = [&] {
            // Newest first, so a refused batch leaves the project exactly as it was. Each of
            // these inverses was built from the state it reverses and is being applied to that
            // same state, so it cannot fail -- and if it somehow did there would be nothing
            // useful left to do about it.
            for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                for (const Command& inv : *step)
                    static_cast<void>(apply(p, inv));
        };

        for (const Command& member : c.body->commands) {
            auto applied = apply(p, member);
            if (!applied) {
                unwind();
                return std::unexpected(std::format("{}: {}", command_name(member),
                                                   applied.error()));
            }
            // A batch cannot hold a batch, so a member whose inverse is one is spliced flat.
            // Its own members keep their order: they were built to run in it.
            if (auto* nested = std::get_if<Batch>(&applied->inverse); nested && nested->body)
                steps.push_back(nested->body->commands);
            else
                steps.push_back({std::move(applied->inverse)});
        }

        // Reversed: the last thing done is the first thing undone.
        std::vector<Command> inverses;
        for (auto step = steps.rbegin(); step != steps.rend(); ++step)
            inverses.insert(inverses.end(), step->begin(), step->end());
        return Applied{make_batch(std::move(inverses), c.description), describe(c)};
    }

    std::expected<Applied, std::string> operator()(const SetProjectName& c) const {
        Applied out{SetProjectName{p.name}, describe(c)};
        p.name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddScene& c) const {
        if (c.index > p.scenes.size())
            return std::unexpected(std::format("scene position {} is past the end", c.index));
        auto existing = all_ids(p);
        std::vector<Id> incoming{c.scene.id};
        if (c.scene.effects.size() > 64)
            return std::unexpected("a filter stack supports at most 64 filters");
        for (const auto& effect : c.scene.effects) {
            if (effect.type.empty())
                return std::unexpected("a filter needs a type");
            incoming.push_back(effect.id);
        }
        collect_defined_ids(c.scene.roots, incoming);
        for (const Id& id : incoming) {
            if (id.is_nil())
                return std::unexpected("the scene contains something with no id");
            if (!existing.insert(id).second)
                return std::unexpected(
                    std::format("id {} is already in the project", id.to_string()));
        }
        // A scene has to live on a canvas that exists: unlike a deck's program scene, there is
        // no sensible degraded state where it simply shows nothing (ADR-0027).
        if (!p.canvas(c.scene.canvas))
            return std::unexpected(std::format("there is no canvas {} for this scene to belong "
                                               "to",
                                               c.scene.canvas.to_string()));
        Applied out{RemoveScene{c.scene.id}, describe(c)};
        p.scenes.insert(p.scenes.begin() + static_cast<std::ptrdiff_t>(c.index), c.scene);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveScene& c) const {
        const auto it = std::find_if(p.scenes.begin(), p.scenes.end(),
                                     [&c](const Scene& s) { return s.id == c.scene; });
        if (it == p.scenes.end())
            return std::unexpected(std::format("there is no scene {}", c.scene.to_string()));
        const auto index = static_cast<std::size_t>(it - p.scenes.begin());

        // Every link naming this scene goes with it, because a link to a scene that is not there
        // is not a link (ADR-0027: one list is one place to prune). They are part of what the
        // removal destroyed, so they are part of what undo has to put back -- which makes the
        // inverse a batch rather than a bare AddScene.
        // Ascending, which is the only order that reinserts them where they were: putting the
        // one from index 5 back before the one from index 2 would be inserting past the end of a
        // list that is still two short.
        std::vector<Command> restore{AddScene{*it, index}};
        for (std::size_t i = 0; i < p.scene_links.size(); ++i) {
            const SceneLink& l = p.scene_links[i];
            if (l.from == c.scene || l.to == c.scene)
                restore.push_back(AddSceneLink{l, i});
        }
        std::string what = std::format("Remove scene {}", quoted(it->name));

        Command inverse = restore.size() == 1 ? std::move(restore.front())
                                              : make_batch(std::move(restore), what);
        Applied out{std::move(inverse), what};
        p.scenes.erase(it);
        std::erase_if(p.scene_links, [&c](const SceneLink& l) {
            return l.from == c.scene || l.to == c.scene;
        });
        return out;
    }

    std::expected<Applied, std::string> operator()(const MoveScene& c) const {
        const auto it = std::find_if(p.scenes.begin(), p.scenes.end(),
                                     [&c](const Scene& s) { return s.id == c.scene; });
        if (it == p.scenes.end())
            return std::unexpected(std::format("there is no scene {}", c.scene.to_string()));
        const auto from = static_cast<std::size_t>(it - p.scenes.begin());
        const std::size_t to = std::min(c.index, p.scenes.size() - 1);
        if (from == to)
            return Applied{MoveScene{c.scene, from}, describe(c)};

        Applied out{MoveScene{c.scene, from}, describe(c)};
        Scene moved = std::move(*it);
        p.scenes.erase(it);
        p.scenes.insert(p.scenes.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
        return out;
    }

    std::expected<Applied, std::string> operator()(const RenameScene& c) const {
        auto s = find_scene(p, c.scene);
        if (!s)
            return std::unexpected(s.error());
        Applied out{RenameScene{c.scene, (*s)->name}, describe(c)};
        (*s)->name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());

        const auto existing = all_ids(p);
        std::vector<Id> incoming;
        collect_defined_ids({c.node}, incoming);
        for (const Id& id : incoming) {
            if (id.is_nil())
                return std::unexpected("the node, or something on it, has no id");
            if (existing.contains(id))
                return std::unexpected(
                    std::format("id {} is already in the project", id.to_string()));
        }

        std::vector<Node>* target = &(*scene)->roots;
        if (c.parent) {
            Node* parent = find_node((*scene)->roots, *c.parent);
            if (!parent)
                return std::unexpected(
                    std::format("there is no node {} to add to", c.parent->to_string()));
            target = parent->children();
            if (!target)
                return std::unexpected(
                    std::format("{} is not a group, so nothing can be added to it",
                                quoted(parent->name)));
        }
        if (c.index > target->size())
            return std::unexpected(std::format("position {} is past the end", c.index));

        // A precomp that reaches its own scene again is a scene that cannot be rendered at
        // all, so it is refused here rather than only reported by validate(). Every other
        // reference in this model degrades -- a node bound to a deleted source draws nothing
        // -- but this one has no bottom: the renderer would recurse until it died.
        if (const auto loop = precomp_cycle_reason(p, c.scene, c.node))
            return std::unexpected(*loop);

        Applied out{RemoveNode{c.scene, c.node.id}, describe(c)};
        target->insert(target->begin() + static_cast<std::ptrdiff_t>(c.index), c.node);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto at = locate((*scene)->roots, c.node);
        if (!at)
            return std::unexpected(std::format("there is no node {} in scene {}",
                                               c.node.to_string(), quoted((*scene)->name)));
        Node removed = (*at->siblings)[at->index];
        Applied out{AddNode{c.scene, at->parent, at->index, removed},
                    std::format("Remove {}", quoted(removed.name))};
        at->siblings->erase(at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index));
        return out;
    }

    std::expected<Applied, std::string> operator()(const MoveNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto at = locate((*scene)->roots, c.node);
        if (!at)
            return std::unexpected(std::format("there is no node {} in scene {}",
                                               c.node.to_string(), quoted((*scene)->name)));

        // Moving a group into itself would detach the whole subtree from the scene and leave the
        // tree referring to nodes nothing owns. Refuse it here rather than discover it later.
        if (c.parent) {
            const Node& moving = (*at->siblings)[at->index];
            if (*c.parent == c.node || is_within(moving, *c.parent))
                return std::unexpected(
                    std::format("{} cannot be moved inside itself", quoted(moving.name)));
        }

        const Applied out{MoveNode{c.scene, c.node, at->parent, at->index},
                          std::format("Move {}",
                                      quoted((*at->siblings)[at->index].name))};

        Node moved = std::move((*at->siblings)[at->index]);
        at->siblings->erase(at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index));

        std::vector<Node>* target = &(*scene)->roots;
        if (c.parent) {
            Node* parent = find_node((*scene)->roots, *c.parent);
            if (!parent || !parent->children()) {
                // Put it back; nothing may be half-applied.
                at->siblings->insert(
                    at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index),
                    std::move(moved));
                return std::unexpected(
                    std::format("there is no group {} to move into", c.parent->to_string()));
            }
            target = parent->children();
        }
        const std::size_t index = std::min(c.index, target->size());
        target->insert(target->begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
        return out;
    }

    std::expected<Applied, std::string> operator()(const RenameNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{RenameNode{c.scene, c.node, (*n)->name}, describe(c)};
        (*n)->name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeVisible& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeVisible{c.scene, c.node, (*n)->visible},
                    std::format("{} {}", c.visible ? "Show" : "Hide",
                                quoted(node_label(p, c.scene, c.node)))};
        (*n)->visible = c.visible;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeLocked& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeLocked{c.scene, c.node, (*n)->locked}, describe(c)};
        (*n)->locked = c.locked;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeTransform& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeTransform{c.scene, c.node, (*n)->transform}, describe(c)};
        (*n)->transform = c.transform;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeLayout& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeLayout{c.scene, c.node, (*n)->layout}, describe(c)};
        (*n)->layout = c.layout;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeBlend& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        if (c.blend.opacity < 0.0 || c.blend.opacity > 1.0)
            return std::unexpected(std::format("opacity {} is outside 0..1", c.blend.opacity));
        Applied out{SetNodeBlend{c.scene, c.node, (*n)->blend}, describe(c)};
        (*n)->blend = c.blend;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeAudio& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        // Only a source node has somewhere to put this. A group has no audio field at all, so there
        // would be nothing for the inverse to restore -- that is the model refusing the state,
        // not this command inventing a rule (ADR-0023).
        auto* l = std::get_if<SourceData>(&(*n)->data);
        if (!l)
            return std::unexpected(
                std::format("{} has no audio of its own to route", quoted((*n)->name)));
        // The bus is a reference, not a precondition: a source node routed at a bus that no
        // longer exists is a warning from validate(), so refusing it here would leave an undo that
        // cannot be applied. A send to itself is structural, and is refused.
        for (const Send& send : c.audio.sends) {
            if (send.to == c.node)
                return std::unexpected("a source node cannot send to itself");
        }
        Applied out{SetNodeAudio{c.scene, c.node, l->audio}, describe(c)};
        l->audio = c.audio;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEffects& c) const {
        auto* stack = p.effect_stack(c.owner);
        if (!stack)
            return std::unexpected("filter owner does not exist");
        if (c.effects.size() > 64)
            return std::unexpected("a filter stack supports at most 64 filters");
        auto ids = all_ids(p);
        for (const auto& effect : *stack)
            ids.erase(effect.id);
        for (const auto& effect : c.effects) {
            if (effect.id.is_nil() || effect.type.empty() || !ids.insert(effect.id).second)
                return std::unexpected("filters need unique IDs and a type");
        }
        Applied out{SetEffects{c.owner, *stack}, describe(c)};
        *stack = c.effects;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddEffect& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        if (c.effect.type.empty())
            return std::unexpected("an effect needs a type");
        if (c.effect.id.is_nil())
            return std::unexpected("an effect needs an id");
        if (all_ids(p).contains(c.effect.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.effect.id.to_string()));
        if (c.index > (*n)->effects.size())
            return std::unexpected(std::format("position {} is past the end", c.index));

        Applied out{RemoveEffect{c.scene, c.node, c.effect.id}, describe(c)};
        (*n)->effects.insert((*n)->effects.begin() + static_cast<std::ptrdiff_t>(c.index),
                             c.effect);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveEffect& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        const auto it = std::find_if((*n)->effects.begin(), (*n)->effects.end(),
                                     [&c](const Effect& e) { return e.id == c.effect; });
        if (it == (*n)->effects.end())
            return std::unexpected(std::format("there is no effect {} on {}",
                                               c.effect.to_string(), quoted((*n)->name)));
        const auto index = static_cast<std::size_t>(it - (*n)->effects.begin());
        Applied out{AddEffect{c.scene, c.node, index, *it},
                    std::format("Remove effect {}", quoted(it->type))};
        (*n)->effects.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEffectParams& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto e = find_effect(**n, c.effect);
        if (!e)
            return std::unexpected(e.error());
        Applied out{SetEffectParams{c.scene, c.node, c.effect, (*e)->params}, describe(c)};
        (*e)->params = c.params;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEffectEnabled& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto e = find_effect(**n, c.effect);
        if (!e)
            return std::unexpected(e.error());
        Applied out{SetEffectEnabled{c.scene, c.node, c.effect, (*e)->enabled}, describe(c)};
        (*e)->enabled = c.enabled;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeSource& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto* placed = std::get_if<SourceData>(&(*n)->data);
        if (!placed)
            return std::unexpected(
                std::format("{} is not a source node, so nothing can be bound to it",
                            quoted((*n)->name)));
        // The source is a reference, not a precondition: see the note on Apply.
        Applied out{SetNodeSource{c.scene, c.node, placed->source}, describe(c)};
        placed->source = c.source;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddSource& c) const {
        if (c.index > p.sources.size())
            return std::unexpected(std::format("source position {} is past the end", c.index));
        if (c.source.id.is_nil())
            return std::unexpected("a source needs an id");
        if (c.source.type.empty())
            return std::unexpected("a source needs a type");
        if (all_ids(p).contains(c.source.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.source.id.to_string()));
        if (c.source.effects.size() > 64)
            return std::unexpected("a filter stack supports at most 64 filters");
        auto ids = all_ids(p);
        ids.insert(c.source.id);
        for (const auto& effect : c.source.effects)
            if (effect.id.is_nil() || effect.type.empty() || !ids.insert(effect.id).second)
                return std::unexpected("filters need unique IDs and a type");
        Applied out{RemoveSource{c.source.id}, describe(c)};
        p.sources.insert(p.sources.begin() + static_cast<std::ptrdiff_t>(c.index), c.source);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveSource& c) const {
        const auto it = std::find_if(p.sources.begin(), p.sources.end(),
                                     [&c](const Source& s) { return s.id == c.source; });
        if (it == p.sources.end())
            return std::unexpected(std::format("there is no source {}", c.source.to_string()));
        const auto index = static_cast<std::size_t>(it - p.sources.begin());
        // Source nodes still bound to it are left pointing at nothing, which validate() reports
        // as a warning. Refusing the removal instead would mean a source could never be deleted
        // while anything used it -- see ADR-0023.
        Applied out{AddSource{*it, index}, std::format("Remove source {}", quoted(it->name))};
        p.sources.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetSource& c) const {
        const auto it = std::find_if(p.sources.begin(), p.sources.end(),
                                     [&c](const Source& s) { return s.id == c.value.id; });
        if (it == p.sources.end())
            return std::unexpected(
                std::format("there is no source {}", c.value.id.to_string()));
        if (c.value.type.empty())
            return std::unexpected("a source needs a type");
        if (c.value.effects.size() > 64)
            return std::unexpected("a filter stack supports at most 64 filters");
        auto ids = all_ids(p);
        for (const auto& effect : it->effects)
            ids.erase(effect.id);
        for (const auto& effect : c.value.effects)
            if (effect.id.is_nil() || effect.type.empty() || !ids.insert(effect.id).second)
                return std::unexpected("filters need unique IDs and a type");
        Applied out{SetSource{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddSceneLink& c) const {
        if (c.index > p.scene_links.size())
            return std::unexpected(std::format("link position {} is past the end", c.index));
        if (c.link.id.is_nil())
            return std::unexpected("a scene link needs an id");
        if (all_ids(p).contains(c.link.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.link.id.to_string()));

        const Scene* from = p.scene(c.link.from);
        if (!from)
            return std::unexpected(
                std::format("there is no scene {} to link from", c.link.from.to_string()));
        const Scene* to = p.scene(c.link.to);
        if (!to)
            return std::unexpected(
                std::format("there is no scene {} to link to", c.link.to.to_string()));

        // A link always runs *from* Main. Linking is a secondary canvas's function: a
        // secondary scene's list answers "which Main scenes bring me up", and Main's own
        // scene rows carry no link control at all. So a link whose source is not on Main is
        // not a link this model has a meaning for -- which is what ADR-0023 asks before a
        // command refuses anything. An earlier reading left this to the panel; it is a rule
        // about what a link *is*, not about which links are worth making.
        if (p.canvases.empty())
            return std::unexpected("a scene link needs a canvas to run from");
        if (from->canvas != p.canvases.front().id)
            return std::unexpected(std::format(
                "{} is not on {}; a link always runs from a Main scene to a scene on another "
                "canvas",
                quoted(from->name), quoted(p.canvases.front().name)));

        if (from->canvas == to->canvas)
            return std::unexpected(std::format(
                "{} and {} are on the same canvas, so a link between them would be a scene "
                "activating itself",
                quoted(from->name), quoted(to->name)));

        for (const SceneLink& existing : p.scene_links) {
            if (existing.from != c.link.from)
                continue;
            const Scene* other = p.scene(existing.to);
            if (other && other->canvas == to->canvas)
                return std::unexpected(std::format(
                    "{} already cues {} on that canvas; a scene can cue at most one scene per "
                    "canvas, or there is no answer to which one",
                    quoted(from->name), quoted(other->name)));
        }

        const Canvas* target = p.canvas(to->canvas);
        if (target && target->emergency_scene && *target->emergency_scene == c.link.to)
            return std::unexpected(std::format(
                "{} is the emergency scene for canvas {}; a safety net that something else can "
                "fire is not one",
                quoted(to->name), quoted(target->name)));

        Applied out{RemoveSceneLink{c.link.id}, describe(c)};
        p.scene_links.insert(p.scene_links.begin() + static_cast<std::ptrdiff_t>(c.index),
                             c.link);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveSceneLink& c) const {
        const auto it = std::find_if(p.scene_links.begin(), p.scene_links.end(),
                                     [&c](const SceneLink& l) { return l.id == c.link; });
        if (it == p.scene_links.end())
            return std::unexpected(
                std::format("there is no scene link {}", c.link.to_string()));
        const auto index = static_cast<std::size_t>(it - p.scene_links.begin());
        Applied out{AddSceneLink{*it, index}, describe(c)};
        p.scene_links.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddCanvas& c) const {
        if (c.index > p.canvases.size())
            return std::unexpected(std::format("canvas position {} is past the end", c.index));
        // "The first one in the list" is only an honest rule for Main while nothing can take
        // that place from it, so the positional rule and the immutability ship together
        // (ADR-0027). Refused here rather than hidden in the UI: ADR-0023.
        if (c.index == 0 && !p.canvases.empty())
            return std::unexpected("Main is the first canvas and nothing can be put before it");
        if (c.canvas.id.is_nil())
            return std::unexpected("a canvas needs an id");
        if (c.canvas.width <= 0 || c.canvas.height <= 0)
            return std::unexpected(
                std::format("a canvas cannot be {}x{}", c.canvas.width, c.canvas.height));
        if (!c.canvas.fps.valid())
            return std::unexpected("a canvas needs a valid frame rate");
        if (all_ids(p).contains(c.canvas.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.canvas.id.to_string()));
        Applied out{RemoveCanvas{c.canvas.id}, describe(c)};
        p.canvases.insert(p.canvases.begin() + static_cast<std::ptrdiff_t>(c.index), c.canvas);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveCanvas& c) const {
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.canvas; });
        if (it == p.canvases.end())
            return std::unexpected(std::format("there is no canvas {}", c.canvas.to_string()));
        if (it == p.canvases.begin())
            return std::unexpected("Main cannot be removed");
        // A scene, deck or output without a canvas is an *error* rather than a warning -- it has
        // no output surface at all -- so this is a precondition and not a reference.
        for (const Scene& sc : p.scenes) {
            if (sc.canvas == c.canvas)
                return std::unexpected(std::format(
                    "scene '{}' belongs to this canvas; move or delete it first", sc.name));
        }
        for (const Deck& d : p.decks) {
            if (d.canvas == c.canvas)
                return std::unexpected(std::format(
                    "deck '{}' renders to this canvas; point it at another one first", d.name));
        }
        for (const Encode& e : p.encodes) {
            if (e.canvas == c.canvas)
                return std::unexpected(std::format(
                    "encode '{}' encodes this canvas; point it at another one first", e.name));
        }
        const auto index = static_cast<std::size_t>(it - p.canvases.begin());
        Applied out{AddCanvas{*it, index}, std::format("Remove canvas {}", quoted(it->name))};
        p.canvases.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetCanvas& c) const {
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.value.id; });
        if (it == p.canvases.end())
            return std::unexpected(
                std::format("there is no canvas {}", c.value.id.to_string()));
        if (c.value.width <= 0 || c.value.height <= 0)
            return std::unexpected(
                std::format("a canvas cannot be {}x{}", c.value.width, c.value.height));
        if (!c.value.fps.valid())
            return std::unexpected("a canvas needs a valid frame rate");
        // Main's size, rate and emergency scene are all editable; only its name is not, because
        // the whole shell says "Main" when it means canvases[0] (ADR-0027).
        if (it == p.canvases.begin() && c.value.name != it->name)
            return std::unexpected("Main cannot be renamed");
        Applied out{SetCanvas{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddBus& c) const {
        if (c.index > p.buses.size())
            return std::unexpected(std::format("bus position {} is past the end", c.index));
        if (c.bus.id.is_nil())
            return std::unexpected("a bus needs an id");
        if (c.bus.channels <= 0)
            return std::unexpected(std::format("a bus cannot have {} channels", c.bus.channels));
        if (all_ids(p).contains(c.bus.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.bus.id.to_string()));
        Applied out{RemoveBus{c.bus.id}, describe(c)};
        p.buses.insert(p.buses.begin() + static_cast<std::ptrdiff_t>(c.index), c.bus);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveBus& c) const {
        const auto it = std::find_if(p.buses.begin(), p.buses.end(),
                                     [&c](const Bus& b) { return b.id == c.bus; });
        if (it == p.buses.end())
            return std::unexpected(std::format("there is no bus {}", c.bus.to_string()));
        // An audio encode of this bus is the same ownership question RemoveEncode answers:
        // a source route left dangling still draws nothing, but a bitstream with nothing to
        // encode is not a degraded state the model has a meaning for. Refuse, as that does.
        for (const AudioEncode& e : p.audio_encodes) {
            if (e.bus == c.bus)
                return std::unexpected(
                    std::format("audio encode '{}' encodes this bus; point it at another one "
                                "first",
                                e.name));
        }
        // Keep source routes visibly unresolved, but remove incoming bus sends: those
        // otherwise leave an invalid graph, particularly on older OBS imports.
        const auto index = static_cast<std::size_t>(it - p.buses.begin());
        Applied out{AddBus{*it, index}, std::format("Remove bus {}", quoted(it->name))};
        std::vector<Command> restore{out.inverse};
        for (auto& bus : p.buses) {
            if (bus.id == c.bus ||
                std::ranges::none_of(bus.sends, [&](const Send& send) { return send.to == c.bus; }))
                continue;
            restore.push_back(SetBus{bus});
            std::erase_if(bus.sends, [&](const Send& send) { return send.to == c.bus; });
        }
        if (restore.size() > 1)
            out.inverse = make_batch(std::move(restore), "Restore bus and sends");
        p.buses.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetBus& c) const {
        const auto it = std::find_if(p.buses.begin(), p.buses.end(),
                                     [&c](const Bus& b) { return b.id == c.value.id; });
        if (it == p.buses.end())
            return std::unexpected(std::format("there is no bus {}", c.value.id.to_string()));
        if (c.value.channels <= 0)
            return std::unexpected(
                std::format("a bus cannot have {} channels", c.value.channels));
        for (const Send& s : c.value.sends) {
            if (s.to == c.value.id)
                return std::unexpected("a bus cannot send to itself");
        }
        Applied out{SetBus{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddEncode& c) const {
        if (c.index > p.encodes.size())
            return std::unexpected(std::format("encode position {} is past the end", c.index));
        if (c.encode.id.is_nil())
            return std::unexpected("an encode needs an id");
        if (c.encode.encoder.bitrate_kbps <= 0)
            return std::unexpected(
                std::format("a bitrate of {} kbps is not usable", c.encode.encoder.bitrate_kbps));
        if (c.encode.rescale && (c.encode.rescale->x <= 0.0 || c.encode.rescale->y <= 0.0))
            return std::unexpected(std::format("an encode cannot rescale to {}x{}",
                                               c.encode.rescale->x, c.encode.rescale->y));
        // Nil is a deliberately disconnected graph input; non-nil references must resolve.
        if (!c.encode.canvas.is_nil() && !p.canvas(c.encode.canvas))
            return std::unexpected(
                std::format("there is no canvas {} to encode", c.encode.canvas.to_string()));
        if (all_ids(p).contains(c.encode.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.encode.id.to_string()));
        Applied out{RemoveEncode{c.encode.id}, describe(c)};
        p.encodes.insert(p.encodes.begin() + static_cast<std::ptrdiff_t>(c.index), c.encode);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveEncode& c) const {
        const auto it = std::find_if(p.encodes.begin(), p.encodes.end(),
                                     [&c](const Encode& e) { return e.id == c.encode; });
        if (it == p.encodes.end())
            return std::unexpected(std::format("there is no encode {}", c.encode.to_string()));
        for (const Output& o : p.outputs) {
            if (o.encode == c.encode)
                return std::unexpected(std::format(
                    "output '{}' sends this encode; point it at another one first", o.name));
        }
        const auto index = static_cast<std::size_t>(it - p.encodes.begin());
        Applied out{AddEncode{*it, index}, std::format("Remove encode {}", quoted(it->name))};
        p.encodes.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEncode& c) const {
        const auto it = std::find_if(p.encodes.begin(), p.encodes.end(),
                                     [&c](const Encode& e) { return e.id == c.value.id; });
        if (it == p.encodes.end())
            return std::unexpected(std::format("there is no encode {}", c.value.id.to_string()));
        if (c.value.encoder.bitrate_kbps <= 0)
            return std::unexpected(
                std::format("a bitrate of {} kbps is not usable", c.value.encoder.bitrate_kbps));
        if (c.value.rescale && (c.value.rescale->x <= 0.0 || c.value.rescale->y <= 0.0))
            return std::unexpected(std::format("an encode cannot rescale to {}x{}",
                                               c.value.rescale->x, c.value.rescale->y));
        if (!c.value.canvas.is_nil() && !p.canvas(c.value.canvas))
            return std::unexpected(
                std::format("there is no canvas {} to encode", c.value.canvas.to_string()));
        Applied out{SetEncode{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddAudioEncode& c) const {
        if (c.index > p.audio_encodes.size())
            return std::unexpected(
                std::format("audio encode position {} is past the end", c.index));
        if (c.encode.id.is_nil())
            return std::unexpected("an audio encode needs an id");
        if (c.encode.bitrate_kbps <= 0)
            return std::unexpected(
                std::format("a bitrate of {} kbps is not usable", c.encode.bitrate_kbps));
        // A disconnected input may be saved and reconnected later.
        if (!c.encode.bus.is_nil() && !p.bus(c.encode.bus))
            return std::unexpected(
                std::format("there is no bus {} to encode", c.encode.bus.to_string()));
        if (all_ids(p).contains(c.encode.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.encode.id.to_string()));
        Applied out{RemoveAudioEncode{c.encode.id}, describe(c)};
        p.audio_encodes.insert(p.audio_encodes.begin() + static_cast<std::ptrdiff_t>(c.index),
                               c.encode);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveAudioEncode& c) const {
        const auto it = std::find_if(p.audio_encodes.begin(), p.audio_encodes.end(),
                                     [&c](const AudioEncode& e) { return e.id == c.encode; });
        if (it == p.audio_encodes.end())
            return std::unexpected(
                std::format("there is no audio encode {}", c.encode.to_string()));
        for (const Output& o : p.outputs) {
            if (o.audio_encode && *o.audio_encode == c.encode)
                return std::unexpected(std::format(
                    "output '{}' sends this audio encode; point it at another one first",
                    o.name));
        }
        const auto index = static_cast<std::size_t>(it - p.audio_encodes.begin());
        Applied out{AddAudioEncode{*it, index},
                    std::format("Remove audio encode {}", quoted(it->name))};
        p.audio_encodes.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetAudioEncode& c) const {
        const auto it = std::find_if(p.audio_encodes.begin(), p.audio_encodes.end(),
                                     [&c](const AudioEncode& e) { return e.id == c.value.id; });
        if (it == p.audio_encodes.end())
            return std::unexpected(
                std::format("there is no audio encode {}", c.value.id.to_string()));
        if (c.value.bitrate_kbps <= 0)
            return std::unexpected(
                std::format("a bitrate of {} kbps is not usable", c.value.bitrate_kbps));
        if (!c.value.bus.is_nil() && !p.bus(c.value.bus))
            return std::unexpected(
                std::format("there is no bus {} to encode", c.value.bus.to_string()));
        Applied out{SetAudioEncode{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddOutput& c) const {
        if (c.index > p.outputs.size())
            return std::unexpected(std::format("output position {} is past the end", c.index));
        if (c.output.id.is_nil())
            return std::unexpected("an output needs an id");
        // A disconnected video input is distinct from a dangling encode reference.
        if (!c.output.encode.is_nil() && !p.encode(c.output.encode))
            return std::unexpected(
                std::format("there is no encode {} to send", c.output.encode.to_string()));
        if (c.output.audio_encode && !p.audio_encode(*c.output.audio_encode))
            return std::unexpected(std::format("there is no audio encode {} to send",
                                               c.output.audio_encode->to_string()));
        if (all_ids(p).contains(c.output.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.output.id.to_string()));
        Applied out{RemoveOutput{c.output.id}, describe(c)};
        p.outputs.insert(p.outputs.begin() + static_cast<std::ptrdiff_t>(c.index), c.output);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveOutput& c) const {
        const auto it = std::find_if(p.outputs.begin(), p.outputs.end(),
                                     [&c](const Output& o) { return o.id == c.output; });
        if (it == p.outputs.end())
            return std::unexpected(std::format("there is no output {}", c.output.to_string()));
        const auto index = static_cast<std::size_t>(it - p.outputs.begin());
        Applied out{AddOutput{*it, index}, std::format("Remove output {}", quoted(it->name))};
        p.outputs.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetOutput& c) const {
        const auto it = std::find_if(p.outputs.begin(), p.outputs.end(),
                                     [&c](const Output& o) { return o.id == c.value.id; });
        if (it == p.outputs.end())
            return std::unexpected(
                std::format("there is no output {}", c.value.id.to_string()));
        if (!c.value.encode.is_nil() && !p.encode(c.value.encode))
            return std::unexpected(
                std::format("there is no encode {} to send", c.value.encode.to_string()));
        if (c.value.audio_encode && !p.audio_encode(*c.value.audio_encode))
            return std::unexpected(std::format("there is no audio encode {} to send",
                                               c.value.audio_encode->to_string()));
        if (c.value.enabled && c.value.destination.url.empty() &&
            c.value.destination.kind != DestinationKind::Replay)
            return std::unexpected("an output cannot be enabled with nowhere to send to");
        Applied out{SetOutput{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetProjectSettings& c) const {
        if (c.value.sample_rate <= 0)
            return std::unexpected(
                std::format("a sample rate of {} is not usable", c.value.sample_rate));
        if (c.value.render_queue_frames < 1)
            return std::unexpected("the render queue needs at least one frame");
        Applied out{SetProjectSettings{p.settings}, describe(c)};
        p.settings = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetSceneFavourite& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        Applied out{SetSceneFavourite{c.scene, (*scene)->favourite}, describe(c)};
        (*scene)->favourite = c.favourite;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddDeck& c) const {
        if (c.index > p.decks.size() || c.value.id.is_nil() || all_ids(p).contains(c.value.id))
            return std::unexpected("invalid or duplicate deck identity or position");
        if (!p.canvas(c.value.canvas) || c.value.transition_ms < 0)
            return std::unexpected("a deck needs a canvas and a nonnegative transition duration");
        for (const auto& deck : p.decks)
            if (deck.canvas == c.value.canvas)
                return std::unexpected("this canvas already has a deck");
        Applied out{RemoveDeck{c.value.id}, describe(c)};
        p.decks.insert(p.decks.begin() + static_cast<std::ptrdiff_t>(c.index), c.value);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveDeck& c) const {
        const auto it = std::find_if(p.decks.begin(), p.decks.end(),
                                    [&](const Deck& deck) { return deck.id == c.deck; });
        if (it == p.decks.end()) return std::unexpected("there is no such deck");
        Applied out{AddDeck{*it, static_cast<std::size_t>(it - p.decks.begin())}, describe(c)};
        p.decks.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetDeck& c) const {
        auto d = find_deck(p, c.value.id);
        if (!d)
            return std::unexpected(d.error());
        // Unlike the scenes it points at, the canvas is a precondition: a deck with no canvas
        // has nowhere to put a program, which validate() calls an error rather than a warning.
        if (!p.canvas(c.value.canvas))
            return std::unexpected(
                std::format("there is no canvas {}", c.value.canvas.to_string()));
        if (c.value.transition_ms < 0)
            return std::unexpected(
                std::format("a transition cannot last {} ms", c.value.transition_ms));
        Applied out{SetDeck{**d}, describe(c)};
        **d = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetDeckProgram& c) const {
        auto d = find_deck(p, c.deck);
        if (!d)
            return std::unexpected(d.error());
        // The scene is a reference, not a precondition: see the note on Apply.
        Applied out{SetDeckProgram{c.deck, (*d)->program_scene}, describe(c)};
        (*d)->program_scene = c.scene;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetCanvasEmergencyScene& c) const {
        // The one state this command refuses, and the reason it is allowed to: "a scene that is
        // already cued" is not a judgement about suitability -- which is the designation flow's
        // job and deliberately not this command's -- but the same structural invariant
        // AddSceneLink enforces from the other side. Without it the forbidden state could be
        // reached simply by doing the two steps in the other order.
        if (c.scene) {
            for (const SceneLink& l : p.scene_links) {
                if (l.to == *c.scene)
                    return std::unexpected(
                        "another scene cues this one, so it cannot also be the safety net");
            }
        }
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.canvas; });
        if (it == p.canvases.end())
            return std::unexpected(std::format("there is no canvas {}", c.canvas.to_string()));
        // The scene is a reference, not a precondition: see the note on Apply.
        Applied out{SetCanvasEmergencyScene{c.canvas, it->emergency_scene}, describe(c)};
        it->emergency_scene = c.scene;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetDeckPreview& c) const {
        auto d = find_deck(p, c.deck);
        if (!d)
            return std::unexpected(d.error());
        Applied out{SetDeckPreview{c.deck, (*d)->preview_scene}, describe(c)};
        (*d)->preview_scene = c.scene;
        return out;
    }
};

} // namespace

bool operator==(const Batch& a, const Batch& b) {
    if (a.description != b.description)
        return false;
    if (a.body == b.body)
        return true;
    if (!a.body || !b.body)
        return false;
    return *a.body == *b.body;
}

Command make_batch(std::vector<Command> commands, std::string description) {
    return Batch{std::make_shared<const BatchBody>(BatchBody{std::move(commands)}),
                 std::move(description)};
}

std::expected<Applied, std::string> apply(Project& project, const Command& command) {
    return std::visit(Apply{project}, command);
}

} // namespace ldoc
