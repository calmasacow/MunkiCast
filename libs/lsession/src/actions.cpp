// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lsession/actions.hpp"

#include <algorithm>
#include <format>

#include <toml++/toml.hpp>

namespace lsession {
namespace {

/// The catalogue.
///
/// Every Global action ships with no default trigger. A default for "start streaming" that
/// collides with something the operator already uses is a way to go live by accident, and asking
/// them to choose one costs a few seconds, once.
const std::vector<ActionDef>& catalogue() {
    static const std::vector<ActionDef> actions = {
        // --- editing, application scope ---------------------------------------------------
        {"edit.undo", "Undo", "Editing", Scope::Application, ArgumentKind::None, "Ctrl+Z"},
        {"edit.redo", "Redo", "Editing", Scope::Application, ArgumentKind::None, "Ctrl+Shift+Z"},
        {"file.save", "Save", "Editing", Scope::Application, ArgumentKind::None, "Ctrl+S"},
        //
        // These four act on the selected source node. Their ids keep the pre-ADR-0028 word
        // because an id is what the operator's bindings file stores: renaming one would quietly
        // unbind a key someone chose, with no migration to carry it over. Only the names, which
        // are what the screen shows, follow the ADR.
        {"layer.delete", "Remove From Scene", "Editing", Scope::Application, ArgumentKind::None,
         "Delete"},
        {"layer.move-forward", "Bring Source Forward", "Editing", Scope::Application,
         ArgumentKind::None, "Ctrl+Up"},
        {"layer.move-back", "Send Source Back", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+Down"},
        {"layer.move-to-front", "Move to Top", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+Home"},
        {"layer.move-to-back", "Move to Bottom", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+End"},
        {"layer.toggle-visible", "Toggle Source Visibility", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        //
        // Copy and paste hold a node or a scene out of *this* project, so they are shell state
        // rather than commands and are performed by the frontend (ADR-0036). They are in the
        // catalogue so they are rebindable like everything else.
        {"edit.copy", "Copy", "Editing", Scope::Application, ArgumentKind::None, "Ctrl+C"},
        {"edit.paste", "Paste", "Editing", Scope::Application, ArgumentKind::None, "Ctrl+V"},
        {"edit.group", "Group Selected Sources", "Editing", Scope::Application,
         ArgumentKind::None, "Ctrl+G"},
        {"edit.ungroup", "Ungroup", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+Shift+G"},
        {"edit.select-all", "Select All Sources", "Editing", Scope::Application,
         ArgumentKind::None, "Ctrl+A"},
        // The canvas menu. Stretch is deliberately unbound: Ctrl+S is Save.
        {"transform.edit", "Edit Transform", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+E"},
        {"transform.copy", "Copy Transform", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+Shift+C"},
        {"transform.paste", "Paste Transform", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+Shift+V"},
        {"transform.reset", "Reset Transform", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+R"},
        {"transform.rotate-cw", "Rotate 90 Degrees CW", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        {"transform.rotate-ccw", "Rotate 90 Degrees CCW", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        {"transform.rotate-180", "Rotate 180 Degrees", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        {"transform.flip-h", "Flip Horizontal", "Editing", Scope::Application, ArgumentKind::None,
         ""},
        {"transform.flip-v", "Flip Vertical", "Editing", Scope::Application, ArgumentKind::None, ""},
        {"transform.fit", "Fit to Screen", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+F"},
        {"transform.stretch", "Stretch to Screen", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        {"transform.center", "Center to Screen", "Editing", Scope::Application, ArgumentKind::None,
         "Ctrl+D"},
        {"transform.center-h", "Center Horizontally", "Editing", Scope::Application,
         ArgumentKind::None, ""},
        {"transform.center-v", "Center Vertically", "Editing", Scope::Application,
         ArgumentKind::None, ""},

        // --- the workspace, application scope ---------------------------------------------
        //
        // Ctrl+Z belongs to the document (ADR-0021). The layout stack keeps its own binding,
        // which is also how Premiere and After Effects treat a workspace change.
        {"workspace.undo", "Undo Panel Change", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Z"},
        {"workspace.redo", "Redo Panel Change", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Shift+Z"},
        {"workspace.reset", "Reset Workspace", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Shift+R"},

        // --- the workspace from the keyboard ----------------------------------------------
        //
        // ADR-0016's docking gesture is entirely pointer-driven. Without these an operator who
        // cannot aim a pointer precisely cannot rearrange the workspace at all, which makes
        // "the layout engine has to be good" (ADR-0015) untrue for them.
        //
        // Focusing and moving are bound by default because they are the navigation; grouping
        // and edge-docking are not, because they are rarer and the key space is not free.
        {"workspace.focus-next", "Focus Next Panel", "Workspace", Scope::Application,
         ArgumentKind::None, "F6"},
        {"workspace.focus-previous", "Focus Previous Panel", "Workspace", Scope::Application,
         ArgumentKind::None, "Shift+F6"},
        {"workspace.focus-left", "Focus Panel Left", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Left"},
        {"workspace.focus-right", "Focus Panel Right", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Right"},
        {"workspace.focus-up", "Focus Panel Up", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Up"},
        {"workspace.focus-down", "Focus Panel Down", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Down"},
        {"workspace.move-left", "Move Panel Left", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Shift+Left"},
        {"workspace.move-right", "Move Panel Right", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Shift+Right"},
        {"workspace.move-up", "Move Panel Up", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Shift+Up"},
        {"workspace.move-down", "Move Panel Down", "Workspace", Scope::Application,
         ArgumentKind::None, "Ctrl+Alt+Shift+Down"},
        {"workspace.tab-with-left", "Group With Panel Left", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.tab-with-right", "Group With Panel Right", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.tab-with-up", "Group With Panel Above", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.tab-with-down", "Group With Panel Below", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.edge-left", "Dock Panel to Left Edge", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.edge-right", "Dock Panel to Right Edge", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.edge-up", "Dock Panel to Top Edge", "Workspace", Scope::Application,
         ArgumentKind::None, ""},
        {"workspace.edge-down", "Dock Panel to Bottom Edge", "Workspace", Scope::Application,
         ArgumentKind::None, ""},

        // --- the show, global scope -------------------------------------------------------
        {"scene.switch", "Switch to Scene", "Scenes", Scope::Global, ArgumentKind::Scene, ""},
        {"deck.transition", "Transition", "Scenes", Scope::Global, ArgumentKind::None, ""},
        {"output.toggle-stream", "Start/Stop Streaming", "Outputs", Scope::Global,
         ArgumentKind::None, ""},
        {"output.toggle-record", "Start/Stop Recording", "Outputs", Scope::Global,
         ArgumentKind::None, ""},
        {"output.toggle-replay-buffer", "Start/Stop Replay Buffer", "Outputs", Scope::Global,
         ArgumentKind::None, ""},
        {"output.save-replay", "Save Replay", "Outputs", Scope::Global, ArgumentKind::None, ""},

        // --- the safe state, global scope (ADR-0025) --------------------------------------
        //
        // The panic button is the trigger that will actually get used: someone walks in, a
        // message lands, a drink goes over. The crash case is the rare one.
        {"safe.panic", "Cut to the Emergency Scene", "Safety", Scope::Global, ArgumentKind::None,
         ""},
        {"safe.resume", "Return to the Live Scene", "Safety", Scope::Global, ArgumentKind::None,
         ""},

        // --- audio, global scope ----------------------------------------------------------
        {"audio.toggle-mute", "Mute/Unmute Microphone", "Audio", Scope::Global,
         ArgumentKind::None, ""},
        {"audio.push-to-talk", "Push to Talk", "Audio", Scope::Global, ArgumentKind::None, ""},
    };
    return actions;
}

constexpr std::string_view kSeparator = ":";

} // namespace

std::string_view to_string(Scope s) {
    return s == Scope::Application ? "application" : "global";
}

std::optional<Scope> scope_from_string(std::string_view s) {
    if (s == "application")
        return Scope::Application;
    if (s == "global")
        return Scope::Global;
    return std::nullopt;
}

const std::vector<ActionDef>& action_catalogue() { return catalogue(); }

const ActionDef* find_action(std::string_view id) {
    const auto& all = catalogue();
    const auto it = std::find_if(all.begin(), all.end(),
                                 [id](const ActionDef& a) { return a.id == id; });
    return it == all.end() ? nullptr : &*it;
}

std::string ActionRef::to_string() const {
    return argument ? std::format("{}{}{}", action, kSeparator, argument->to_string()) : action;
}

std::optional<ActionRef> ActionRef::parse(std::string_view text) {
    const auto at = text.find(kSeparator);
    ActionRef ref;
    if (at == std::string_view::npos) {
        ref.action = std::string{text};
    } else {
        ref.action = std::string{text.substr(0, at)};
        const auto id = ldoc::Id::parse(text.substr(at + 1));
        if (!id)
            return std::nullopt;
        ref.argument = *id;
    }
    if (ref.action.empty())
        return std::nullopt;
    return ref;
}

Resolved resolve(const ActionRef& ref, const ldoc::Project& project) {
    const ActionDef* def = find_action(ref.action);
    if (!def)
        return Resolved{Outcome::Unknown, std::nullopt,
                        std::format("'{}' is not an action this build knows", ref.action)};

    if (def->argument != ArgumentKind::None && !ref.argument)
        return Resolved{Outcome::Unknown, std::nullopt,
                        std::format("'{}' needs something to act on", def->title)};

    if (ref.action == "edit.undo")
        return Resolved{Outcome::Undo, std::nullopt, {}};
    if (ref.action == "edit.redo")
        return Resolved{Outcome::Redo, std::nullopt, {}};
    if (ref.action == "file.save")
        return Resolved{Outcome::Save, std::nullopt, {}};

    if (ref.action == "scene.switch") {
        if (!project.scene(*ref.argument))
            return Resolved{Outcome::Unknown, std::nullopt, "that scene no longer exists"};
        if (project.decks.empty())
            return Resolved{Outcome::Unknown, std::nullopt, "there is no deck to switch"};
        return Resolved{Outcome::Command,
                        ldoc::SetDeckProgram{project.decks.front().id, ref.argument}, {}};
    }

    if (ref.action == "safe.panic") {
        // The deck is pointed at the canvas's emergency scene. It is an ordinary command, so it
        // is journaled and undoable like anything else -- which matters, because the operator
        // will want to get back exactly where they were.
        if (project.decks.empty() || project.canvases.empty())
            return Resolved{Outcome::Unknown, std::nullopt, "there is nothing to cut away from"};
        const auto& canvas = project.canvases.front();
        if (!canvas.emergency_scene)
            return Resolved{Outcome::Unknown, std::nullopt,
                            std::format("canvas '{}' has no emergency scene set", canvas.name)};
        return Resolved{Outcome::Command,
                        ldoc::SetDeckProgram{project.decks.front().id, canvas.emergency_scene},
                        {}};
    }

    // Workspace actions do not touch the document at all -- the layout is not part of it
    // (ADR-0021) -- so they resolve to nothing here and are performed by the shell.
    if (ref.action.starts_with("workspace."))
        return Resolved{Outcome::Workspace, std::nullopt, {}};

    return Resolved{Outcome::NotYetAvailable, std::nullopt,
                    std::format("'{}' is not wired up yet", def->title)};
}

std::vector<ActionRef> available_actions(const ldoc::Project& project) {
    std::vector<ActionRef> out;
    for (const ActionDef& def : catalogue()) {
        switch (def.argument) {
        case ArgumentKind::None:
            out.push_back(ActionRef{def.id, std::nullopt});
            break;
        case ArgumentKind::Scene:
            for (const ldoc::Scene& s : project.scenes)
                out.push_back(ActionRef{def.id, s.id});
            break;
        case ArgumentKind::Canvas:
            for (const ldoc::Canvas& c : project.canvases)
                out.push_back(ActionRef{def.id, c.id});
            break;
        }
    }
    return out;
}

std::string describe(const ActionRef& ref, const ldoc::Project& project) {
    const ActionDef* def = find_action(ref.action);
    if (!def)
        return ref.action;
    if (!ref.argument)
        return def->title;
    if (const ldoc::Scene* s = project.scene(*ref.argument))
        return std::format("{}: {}", def->title, s->name);
    if (const ldoc::Canvas* c = project.canvas(*ref.argument))
        return std::format("{}: {}", def->title, c->name);
    return std::format("{}: (missing)", def->title);
}

// ---- bindings ---------------------------------------------------------------------------------

std::string Bindings::trigger_for(const ActionRef& ref) const {
    const auto it = overrides_.find(ref.to_string());
    if (it != overrides_.end())
        return it->second;
    const ActionDef* def = find_action(ref.action);
    // A parameterised action has no useful default: one default trigger shared by every scene
    // would bind them all to the same key.
    if (!def || def->argument != ArgumentKind::None)
        return {};
    return def->default_trigger;
}

bool Bindings::is_default(const ActionRef& ref) const {
    return !overrides_.contains(ref.to_string());
}

void Bindings::set_trigger(const ActionRef& ref, std::string trigger) {
    overrides_.insert_or_assign(ref.to_string(), std::move(trigger));
}

void Bindings::reset(const ActionRef& ref) { overrides_.erase(ref.to_string()); }

void Bindings::reset_all() { overrides_.clear(); }

std::optional<ActionRef> Bindings::conflict(const ActionRef& ref, std::string_view trigger,
                                            const ldoc::Project& project) const {
    if (trigger.empty())
        return std::nullopt;
    const ActionDef* def = find_action(ref.action);
    if (!def)
        return std::nullopt;

    for (const ActionRef& other : available_actions(project)) {
        if (other == ref)
            continue;
        const ActionDef* other_def = find_action(other.action);
        // Only within a scope. An application shortcut and a global one can share a key: they
        // are delivered by different things, in different circumstances, and refusing that would
        // make the global list needlessly cramped.
        if (!other_def || other_def->scope != def->scope)
            continue;
        if (trigger_for(other) == trigger)
            return other;
    }
    return std::nullopt;
}

std::string Bindings::to_toml() const {
    std::string out;
    out += "# MunkiCast key bindings.\n";
    out += "#\n";
    out += "# Only what differs from the defaults is written here, so a release that changes a\n";
    out += "# default changes it for everyone who never touched that binding, and leaves alone\n";
    out += "# everyone who did. Delete a line to go back to the default; an empty value means\n";
    out += "# deliberately unbound, which is not the same thing.\n\n";
    out += "version = 1\n\n[bindings]\n";
    for (const auto& [action, trigger] : overrides_)
        out += std::format("\"{}\" = \"{}\"\n", action, trigger);
    return out;
}

std::expected<Bindings, std::string> Bindings::from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("line {}: {}", e.source().begin.line,
                                           std::string_view{e.description()}));
    } catch (const std::exception& e) {
        // Same contract as the JSON side: a damaged file is refused, never thrown out of.
        return std::unexpected(e.what());
    } catch (...) {
        return std::unexpected("the file could not be parsed");
    }

    const auto version = tbl["version"].value<std::int64_t>();
    if (!version)
        return std::unexpected("missing 'version'");
    if (*version != 1)
        return std::unexpected(std::format("unsupported bindings version {}", *version));

    Bindings out;
    const toml::table* section = tbl["bindings"].as_table();
    if (!section)
        return out;
    for (const auto& [key, value] : *section) {
        const auto trigger = value.value<std::string>();
        if (!trigger)
            return std::unexpected(
                std::format("the binding for '{}' is not a string", std::string{key.str()}));
        const std::string action{key.str()};
        if (!ActionRef::parse(action))
            return std::unexpected(std::format("'{}' is not an action", action));
        out.overrides_.insert_or_assign(action, *trigger);
    }
    return out;
}

} // namespace lsession
