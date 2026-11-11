// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/command.hpp"
#include "ldoc/document.hpp"

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// Actions, and what is bound to them.
///
/// An action is **data**: an id, a title, a scope and a default trigger. That is the decision
/// ADR-0024 records, and the reason is that everything else about hotkeys follows from where the
/// scope is written down. Declared per action, as a field, scope can later become user-editable
/// by adding an override map. Hard-coded into two dispatch mechanisms, it could not.
///
/// Both scopes end in the same place -- an `ldoc::Command` sent to the same `Session` -- and
/// differ only in who delivers the trigger:
///
/// - **Application** actions are handled by the UI's own key handling and never touch the portal.
///   Undo, delete, rename. Making them global would be actively harmful: Ctrl+Z firing while the
///   operator types in a browser.
/// - **Global** actions are declared to `org.freedesktop.portal.GlobalShortcuts` and must keep
///   working when the UI is dead, which is one of Phase 1's own acceptance tests. The portal
///   session therefore belongs to the session process, not the frontend.
namespace lsession {

enum class Scope : std::uint8_t {
    /// Fires only while MunkiCast has focus.
    Application,
    /// Fires whatever is focused, including a fullscreen game.
    Global,
};

[[nodiscard]] std::string_view to_string(Scope s);
[[nodiscard]] std::optional<Scope> scope_from_string(std::string_view s);

/// What an action needs before it means anything.
///
/// "Switch to scene" is one action with one row per scene, exactly as OBS lists them. Modelling
/// that as an argument rather than as one action per scene is what keeps a binding attached to
/// the scene when it is renamed.
enum class ArgumentKind : std::uint8_t { None, Scene, Canvas };

struct ActionDef {
    std::string id;
    std::string title;
    /// How the bindings table groups them.
    std::string category;
    Scope scope = Scope::Application;
    ArgumentKind argument = ArgumentKind::None;
    /// Empty means unbound by default.
    ///
    /// Every Global action ships unbound on purpose. A default binding for "start streaming" that
    /// happens to collide with something the operator already uses is a way to go live by
    /// accident, and the cost of asking them to choose one is a few seconds, once.
    std::string default_trigger;

    friend bool operator==(const ActionDef&, const ActionDef&) = default;
};

/// Every action this build knows about, in display order.
[[nodiscard]] const std::vector<ActionDef>& action_catalogue();
[[nodiscard]] const ActionDef* find_action(std::string_view id);

/// An action together with the thing it acts on, which is what is actually bound to a key.
struct ActionRef {
    std::string action;
    std::optional<ldoc::Id> argument;

    /// "edit.undo", or "scene.switch:01J0..." -- the form used in the bindings file and on the
    /// wire.
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] static std::optional<ActionRef> parse(std::string_view text);

    friend auto operator<=>(const ActionRef&, const ActionRef&) = default;
    friend bool operator==(const ActionRef&, const ActionRef&) = default;
};

/// What performing an action does.
enum class Outcome : std::uint8_t {
    /// Edits the document; `command` holds it.
    Command,
    Undo,
    Redo,
    Save,
    /// Rearranges the workspace, which is not part of the document (ADR-0021), so the shell
    /// performs it rather than the session.
    Workspace,
    /// A real action with a real binding that no part of this build can perform yet. It is in the
    /// catalogue anyway, so the table is complete and a binding made today survives until the
    /// feature arrives -- rather than the operator having to rebind once it does.
    NotYetAvailable,
    /// The action does not exist, or its argument does not.
    Unknown,
};

struct Resolved {
    Outcome outcome = Outcome::Unknown;
    std::optional<ldoc::Command> command;
    /// Why, when the outcome is NotYetAvailable or Unknown.
    std::string note;
};

/// Turn an action into something that can be done to this project.
[[nodiscard]] Resolved resolve(const ActionRef& ref, const ldoc::Project& project);

/// Every action that currently makes sense, including one row per scene for the parameterised
/// ones. This is what the bindings table lists.
[[nodiscard]] std::vector<ActionRef> available_actions(const ldoc::Project& project);
/// What the bindings table shows in the name column.
[[nodiscard]] std::string describe(const ActionRef& ref, const ldoc::Project& project);

/// Which key is on which action.
///
/// Holds only what differs from the defaults, so a release that changes a default trigger changes
/// it for everyone who never touched it -- and leaves alone everyone who did.
class Bindings {
public:
    [[nodiscard]] std::string trigger_for(const ActionRef& ref) const;
    [[nodiscard]] bool is_default(const ActionRef& ref) const;

    /// An empty trigger unbinds it, which is different from resetting it to the default.
    void set_trigger(const ActionRef& ref, std::string trigger);
    void reset(const ActionRef& ref);
    void reset_all();

    /// What already uses this trigger, so the table can say so instead of silently stealing it.
    /// Only ever conflicts within a scope: an application shortcut and a global one can share a
    /// key, because they are delivered by different things in different circumstances.
    [[nodiscard]] std::optional<ActionRef> conflict(const ActionRef& ref,
                                                    std::string_view trigger,
                                                    const ldoc::Project& project) const;

    [[nodiscard]] std::string to_toml() const;
    [[nodiscard]] static std::expected<Bindings, std::string> from_toml(std::string_view text);

    [[nodiscard]] std::size_t override_count() const { return overrides_.size(); }

private:
    std::map<std::string, std::string> overrides_;
};

} // namespace lsession
