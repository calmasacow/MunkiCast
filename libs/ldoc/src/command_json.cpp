// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/command.hpp"

#include "json_detail.hpp"

#include <format>
#include <string>

namespace ldoc {
namespace {

using detail::Jv;
using detail::Rd;

Jv enc_opt_id(const std::optional<Id>& id) {
    return id ? detail::enc(*id) : Jv{Json::nullValue};
}

Jv enc_index(std::size_t i) { return Jv{static_cast<Json::UInt64>(i)}; }

Jv body(const SetProjectName& c) {
    Jv o{Json::objectValue};
    o["name"] = c.name;
    return o;
}

Jv body(const AddScene& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc_scene(c.scene);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveScene& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    return o;
}

Jv body(const MoveScene& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RenameScene& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["name"] = c.name;
    return o;
}

Jv body(const AddNode& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["parent"] = enc_opt_id(c.parent);
    o["index"] = enc_index(c.index);
    o["node"] = detail::enc(c.node);
    return o;
}

Jv body(const RemoveNode& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    return o;
}

Jv body(const MoveNode& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["parent"] = enc_opt_id(c.parent);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RenameNode& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["name"] = c.name;
    return o;
}

Jv body(const SetNodeVisible& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["visible"] = c.visible;
    return o;
}

Jv body(const SetNodeLocked& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["locked"] = c.locked;
    return o;
}

Jv body(const SetNodeTransform& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["transform"] = detail::enc(c.transform);
    return o;
}

Jv body(const SetNodeLayout& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["layout"] = detail::enc(c.layout);
    return o;
}

Jv body(const SetNodeBlend& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["blend"] = detail::enc(c.blend);
    return o;
}

Jv body(const SetNodeAudio& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["audio"] = detail::enc(c.audio);
    return o;
}

Jv body(const SetEffects& c) {
    Jv out{Json::objectValue};
    out["owner"] = detail::enc(c.owner);
    out["effects"] = Jv{Json::arrayValue};
    for (const auto& effect : c.effects)
        out["effects"].append(detail::enc(effect));
    return out;
}

Jv body(const AddEffect& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["index"] = enc_index(c.index);
    o["effect"] = detail::enc(c.effect);
    return o;
}

Jv body(const RemoveEffect& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["effect"] = detail::enc(c.effect);
    return o;
}

Jv body(const SetEffectParams& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["effect"] = detail::enc(c.effect);
    o["params"] = detail::enc(c.params);
    return o;
}

Jv body(const SetEffectEnabled& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["effect"] = detail::enc(c.effect);
    o["enabled"] = c.enabled;
    return o;
}

Jv body(const SetSceneFavourite& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["favourite"] = c.favourite;
    return o;
}

Jv body(const AddDeck& c) {
    Jv o{Json::objectValue};
    o["deck"] = detail::enc(c.value);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveDeck& c) {
    Jv o{Json::objectValue};
    o["deck"] = detail::enc(c.deck);
    return o;
}

Jv body(const SetDeck& c) {
    Jv o{Json::objectValue};
    o["deck"] = detail::enc(c.value);
    return o;
}

Jv body(const SetDeckProgram& c) {
    Jv o{Json::objectValue};
    o["deck"] = detail::enc(c.deck);
    o["scene"] = enc_opt_id(c.scene);
    return o;
}

Jv body(const SetCanvasEmergencyScene& c) {
    Jv o{Json::objectValue};
    o["canvas"] = detail::enc(c.canvas);
    o["scene"] = enc_opt_id(c.scene);
    return o;
}

Jv body(const SetNodeSource& c) {
    Jv o{Json::objectValue};
    o["scene"] = detail::enc(c.scene);
    o["node"] = detail::enc(c.node);
    o["source"] = enc_opt_id(c.source);
    return o;
}

Jv body(const AddSource& c) {
    Jv o{Json::objectValue};
    o["source"] = detail::enc(c.source);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveSource& c) {
    Jv o{Json::objectValue};
    o["source"] = detail::enc(c.source);
    return o;
}

Jv body(const SetSource& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const AddCanvas& c) {
    Jv o{Json::objectValue};
    o["canvas"] = detail::enc(c.canvas);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveCanvas& c) {
    Jv o{Json::objectValue};
    o["canvas"] = detail::enc(c.canvas);
    return o;
}

Jv body(const SetCanvas& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const AddBus& c) {
    Jv o{Json::objectValue};
    o["bus"] = detail::enc(c.bus);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveBus& c) {
    Jv o{Json::objectValue};
    o["bus"] = detail::enc(c.bus);
    return o;
}

Jv body(const SetBus& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const AddSceneLink& c) {
    Jv o{Json::objectValue};
    o["link"] = detail::enc(c.link);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveSceneLink& c) {
    Jv o{Json::objectValue};
    o["link"] = detail::enc(c.link);
    return o;
}

Jv body(const AddEncode& c) {
    Jv o{Json::objectValue};
    o["encode"] = detail::enc(c.encode);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveEncode& c) {
    Jv o{Json::objectValue};
    o["encode"] = detail::enc(c.encode);
    return o;
}

Jv body(const SetEncode& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const AddAudioEncode& c) {
    Jv o{Json::objectValue};
    o["encode"] = detail::enc(c.encode);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveAudioEncode& c) {
    Jv o{Json::objectValue};
    o["encode"] = detail::enc(c.encode);
    return o;
}

Jv body(const SetAudioEncode& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const AddOutput& c) {
    Jv o{Json::objectValue};
    o["output"] = detail::enc(c.output);
    o["index"] = enc_index(c.index);
    return o;
}

Jv body(const RemoveOutput& c) {
    Jv o{Json::objectValue};
    o["output"] = detail::enc(c.output);
    return o;
}

Jv body(const SetOutput& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const SetProjectSettings& c) {
    Jv o{Json::objectValue};
    o["value"] = detail::enc(c.value);
    return o;
}

Jv body(const SetDeckPreview& c) {
    Jv o{Json::objectValue};
    o["deck"] = detail::enc(c.deck);
    o["scene"] = enc_opt_id(c.scene);
    return o;
}

std::size_t dec_index(Rd& rd, const Jv& o, const char* key, std::string_view path) {
    const std::int64_t v = rd.integer(o, key, path, 0);
    if (v < 0) {
        rd.fail(path, "a position cannot be negative");
        return 0;
    }
    return static_cast<std::size_t>(v);
}

/// Declared before the visitor below, which reaches it by overload resolution.
Jv body(const Batch& c);

Jv command_value(const Command& command) {
    Jv root = std::visit([](const auto& c) { return body(c); }, command);
    root["cmd"] = std::string{command_name(command)};
    return root;
}

/// A batch carries whole commands, not bodies: each member needs its own "cmd" to be read back.
Jv body(const Batch& c) {
    Jv o{Json::objectValue};
    o["description"] = c.description;
    Jv members{Json::arrayValue};
    if (c.body) {
        for (const Command& member : c.body->commands)
            members.append(command_value(member));
    }
    o["commands"] = members;
    return o;
}

} // namespace

std::string command_to_json(const Command& command) {
    return detail::write(command_value(command));
}

namespace {

std::expected<Command, std::string> command_from_value(Rd& rd, const Jv& o,
                                                       std::string_view path);

} // namespace

std::expected<Command, std::string> command_from_json(std::string_view text) {
    std::string parse_error;
    const auto root = detail::parse_text(text, parse_error);
    if (!root)
        return std::unexpected(parse_error);
    if (!root->isObject())
        return std::unexpected("a command is not a JSON object");

    Rd rd;
    auto out = command_from_value(rd, *root, "command");
    if (!out)
        return out;
    if (!rd.ok())
        return std::unexpected(rd.error());
    return out;
}

namespace {

std::expected<Command, std::string> command_from_value(Rd& rd, const Jv& o,
                                                       std::string_view path) {
    if (!o.isObject())
        return std::unexpected("a command is not a JSON object");
    const std::string name = rd.text(o, "cmd", path);
    if (name.empty())
        return std::unexpected("a command has no 'cmd'");

    Command out = SetProjectName{};
    if (name == "project.set-name") {
        out = SetProjectName{rd.text(o, "name", path)};
    } else if (name == "scene.add") {
        const Jv* sv = rd.object(o, "scene", path);
        if (!sv)
            return std::unexpected("scene.add has no scene");
        out = AddScene{detail::dec_scene(rd, *sv, path), dec_index(rd, o, "index", path)};
    } else if (name == "scene.remove") {
        out = RemoveScene{rd.id(o, "scene", path)};
    } else if (name == "scene.move") {
        out = MoveScene{rd.id(o, "scene", path), dec_index(rd, o, "index", path)};
    } else if (name == "scene.rename") {
        out = RenameScene{rd.id(o, "scene", path), rd.text(o, "name", path)};
    } else if (name == "node.add") {
        const Jv* nv = rd.object(o, "node", path);
        if (!nv)
            return std::unexpected("node.add has no node");
        out = AddNode{rd.id(o, "scene", path), rd.id_opt(o, "parent", path),
                      dec_index(rd, o, "index", path), detail::dec_node(rd, *nv, path)};
    } else if (name == "node.remove") {
        out = RemoveNode{rd.id(o, "scene", path), rd.id(o, "node", path)};
    } else if (name == "node.move") {
        out = MoveNode{rd.id(o, "scene", path), rd.id(o, "node", path),
                       rd.id_opt(o, "parent", path), dec_index(rd, o, "index", path)};
    } else if (name == "node.rename") {
        out = RenameNode{rd.id(o, "scene", path), rd.id(o, "node", path),
                         rd.text(o, "name", path)};
    } else if (name == "node.set-visible") {
        out = SetNodeVisible{rd.id(o, "scene", path), rd.id(o, "node", path),
                             rd.boolean(o, "visible", path, true)};
    } else if (name == "node.set-locked") {
        out = SetNodeLocked{rd.id(o, "scene", path), rd.id(o, "node", path),
                            rd.boolean(o, "locked", path, false)};
    } else if (name == "node.set-transform") {
        out = SetNodeTransform{rd.id(o, "scene", path), rd.id(o, "node", path),
                               detail::dec_transform(rd, o, "transform", path)};
    } else if (name == "node.set-layout") {
        out = SetNodeLayout{rd.id(o, "scene", path), rd.id(o, "node", path),
                            detail::dec_layout(rd, o, "layout", path)};
    } else if (name == "node.set-blend") {
        out = SetNodeBlend{rd.id(o, "scene", path), rd.id(o, "node", path),
                           detail::dec_blend(rd, o, "blend", path)};
    } else if (name == "node.set-audio") {
        AudioRouting audio;
        if (const Jv* a = rd.object(o, "audio", path))
            audio = detail::dec_audio_routing(rd, *a, path);
        out = SetNodeAudio{rd.id(o, "scene", path), rd.id(o, "node", path), audio};
    } else if (name == "effects.set") {
        out = SetEffects{rd.id(o, "owner", path), detail::dec_effects(rd, o, "effects", path)};
    } else if (name == "effect.add") {
        const Jv* ev = rd.object(o, "effect", path);
        if (!ev)
            return std::unexpected("effect.add has no effect");
        out = AddEffect{rd.id(o, "scene", path), rd.id(o, "node", path),
                        dec_index(rd, o, "index", path), detail::dec_effect(rd, *ev, path)};
    } else if (name == "effect.remove") {
        out = RemoveEffect{rd.id(o, "scene", path), rd.id(o, "node", path),
                           rd.id(o, "effect", path)};
    } else if (name == "effect.set-params") {
        out = SetEffectParams{rd.id(o, "scene", path), rd.id(o, "node", path),
                              rd.id(o, "effect", path),
                              detail::dec_params(rd, o, "params", path)};
    } else if (name == "effect.set-enabled") {
        out = SetEffectEnabled{rd.id(o, "scene", path), rd.id(o, "node", path),
                               rd.id(o, "effect", path), rd.boolean(o, "enabled", path, true)};
    } else if (name == "scene.set-favourite") {
        out = SetSceneFavourite{rd.id(o, "scene", path),
                                rd.boolean(o, "favourite", path, true)};
    } else if (name == "deck.add") {
        const Jv* d = rd.object(o, "deck", path);
        out = AddDeck{d ? detail::dec_deck(rd, *d, path) : Deck{}, dec_index(rd, o, "index", path)};
    } else if (name == "deck.remove") {
        out = RemoveDeck{rd.id(o, "deck", path)};
    } else if (name == "deck.set") {
        const Jv* d = rd.object(o, "deck", path);
        out = SetDeck{d ? detail::dec_deck(rd, *d, path) : Deck{}};
    } else if (name == "deck.set-program") {
        out = SetDeckProgram{rd.id(o, "deck", path), rd.id_opt(o, "scene", path)};
    } else if (name == "deck.set-preview") {
        out = SetDeckPreview{rd.id(o, "deck", path), rd.id_opt(o, "scene", path)};
    } else if (name == "canvas.set-emergency-scene") {
        out = SetCanvasEmergencyScene{rd.id(o, "canvas", path), rd.id_opt(o, "scene", path)};
    // "layer.set-source" is the version 1 spelling. Still read, never written: the journal
    // is replayed during crash recovery, and refusing an older entry there would lose the
    // work the journal exists to save (ADR-0028).
    } else if (name == "node.set-source" || name == "layer.set-source") {
        out = SetNodeSource{rd.id(o, "scene", path), rd.id(o, "node", path),
                            rd.id_opt(o, "source", path)};
    } else if (name == "source.add") {
        const Jv* sv = rd.object(o, "source", path);
        if (!sv)
            return std::unexpected("source.add has no source");
        out = AddSource{detail::dec_source(rd, *sv, path), dec_index(rd, o, "index", path)};
    } else if (name == "source.remove") {
        out = RemoveSource{rd.id(o, "source", path)};
    } else if (name == "source.set") {
        const Jv* sv = rd.object(o, "value", path);
        if (!sv)
            return std::unexpected("source.set has no value");
        out = SetSource{detail::dec_source(rd, *sv, path)};
    } else if (name == "scene-link.add") {
        const Jv* lv = rd.object(o, "link", path);
        if (!lv)
            return std::unexpected("scene-link.add has no link");
        out = AddSceneLink{detail::dec_scene_link(rd, *lv, path), dec_index(rd, o, "index", path)};
    } else if (name == "scene-link.remove") {
        out = RemoveSceneLink{rd.id(o, "link", path)};
    } else if (name == "canvas.add") {
        const Jv* cv = rd.object(o, "canvas", path);
        if (!cv)
            return std::unexpected("canvas.add has no canvas");
        out = AddCanvas{detail::dec_canvas(rd, *cv, path), dec_index(rd, o, "index", path)};
    } else if (name == "canvas.remove") {
        out = RemoveCanvas{rd.id(o, "canvas", path)};
    } else if (name == "bus.add") {
        const Jv* bv = rd.object(o, "bus", path);
        if (!bv)
            return std::unexpected("bus.add has no bus");
        out = AddBus{detail::dec_bus(rd, *bv, path), dec_index(rd, o, "index", path)};
    } else if (name == "bus.remove") {
        out = RemoveBus{rd.id(o, "bus", path)};
    } else if (name == "bus.set") {
        const Jv* bv = rd.object(o, "value", path);
        if (!bv)
            return std::unexpected("bus.set has no value");
        out = SetBus{detail::dec_bus(rd, *bv, path)};
    } else if (name == "encode.add") {
        const Jv* ev = rd.object(o, "encode", path);
        if (!ev)
            return std::unexpected("encode.add has no encode");
        out = AddEncode{detail::dec_encode(rd, *ev, path), dec_index(rd, o, "index", path)};
    } else if (name == "encode.remove") {
        out = RemoveEncode{rd.id(o, "encode", path)};
    } else if (name == "encode.set") {
        const Jv* ev = rd.object(o, "value", path);
        if (!ev)
            return std::unexpected("encode.set has no value");
        out = SetEncode{detail::dec_encode(rd, *ev, path)};
    } else if (name == "audio-encode.add") {
        const Jv* ev = rd.object(o, "encode", path);
        if (!ev)
            return std::unexpected("audio-encode.add has no encode");
        out = AddAudioEncode{detail::dec_audio_encode(rd, *ev, path),
                             dec_index(rd, o, "index", path)};
    } else if (name == "audio-encode.remove") {
        out = RemoveAudioEncode{rd.id(o, "encode", path)};
    } else if (name == "audio-encode.set") {
        const Jv* ev = rd.object(o, "value", path);
        if (!ev)
            return std::unexpected("audio-encode.set has no value");
        out = SetAudioEncode{detail::dec_audio_encode(rd, *ev, path)};
    } else if (name == "output.add") {
        const Jv* ov = rd.object(o, "output", path);
        if (!ov)
            return std::unexpected("output.add has no output");
        out = AddOutput{detail::dec_output(rd, *ov, path), dec_index(rd, o, "index", path)};
    } else if (name == "output.remove") {
        out = RemoveOutput{rd.id(o, "output", path)};
    } else if (name == "output.set") {
        const Jv* ov = rd.object(o, "value", path);
        if (!ov)
            return std::unexpected("output.set has no value");
        out = SetOutput{detail::dec_output(rd, *ov, path)};
    } else if (name == "project.set-settings") {
        const Jv* sv = rd.object(o, "value", path);
        if (!sv)
            return std::unexpected("project.set-settings has no value");
        out = SetProjectSettings{detail::dec_settings(rd, *sv, path)};
    } else if (name == "canvas.set") {
        const Jv* cv = rd.object(o, "value", path);
        if (!cv)
            return std::unexpected("canvas.set has no value");
        out = SetCanvas{detail::dec_canvas(rd, *cv, path)};
    } else if (name == "batch") {
        std::vector<Command> members;
        if (const Jv* arr = rd.array(o, "commands", path)) {
            for (const Jv& m : *arr) {
                auto parsed = command_from_value(rd, m, path);
                if (!parsed)
                    return std::unexpected(std::format("inside a batch: {}", parsed.error()));
                members.push_back(std::move(*parsed));
            }
        }
        // Validity -- empty, or nested -- is apply()'s to judge, not the parser's. A journal
        // holding a batch this build would refuse must still be *readable*, so that the refusal
        // is reported rather than looking like a corrupt file.
        out = make_batch(std::move(members), rd.text(o, "description", path, std::string{}));
    } else {
        // Not a fallback: a command this build cannot perform must not be quietly skipped, or a
        // journal written by a newer build would replay as a different session than it recorded.
        return std::unexpected(std::format("'{}' is not a command this build knows", name));
    }

    return out;
}

} // namespace

} // namespace ldoc
