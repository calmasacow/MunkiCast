// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Actions and their bindings (ADR-0024).

#include "lsession/actions.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <set>
#include <string>

using namespace lsession;
using ldoc::Id;

namespace {

ldoc::Project show() {
    ldoc::Project p = ldoc::make_default_project("Show");
    p.scenes[0].name = "Intro";
    ldoc::Scene second;
    second.id = Id::generate();
    second.name = "Gameplay";
    p.scenes.push_back(std::move(second));
    return p;
}

const ActionDef& def_of(std::string_view id) {
    const ActionDef* d = find_action(id);
    REQUIRE(d != nullptr);
    return *d;
}

} // namespace

LTEST_CASE(the_catalogue_is_well_formed) {
    std::set<std::string> ids;
    for (const ActionDef& a : action_catalogue()) {
        CHECK_MSG(ids.insert(a.id).second, "'{}' appears twice", a.id);
        CHECK_MSG(!a.title.empty(), "'{}' has no title", a.id);
        CHECK_MSG(!a.category.empty(), "'{}' has no category", a.id);
        CHECK_MSG(a.id.find('.') != std::string::npos,
                  "'{}' is not namespaced, so two features will eventually collide", a.id);
    }
    CHECK(action_catalogue().size() >= 15);
    CHECK(find_action("edit.undo") != nullptr);
    CHECK(find_action("nothing.like-this") == nullptr);
}

LTEST_CASE(every_global_action_ships_unbound) {
    // A default binding for "start streaming" that collides with something the operator already
    // uses is a way to go live by accident. The cost of asking them to choose is a few seconds,
    // once.
    for (const ActionDef& a : action_catalogue()) {
        if (a.scope != Scope::Global)
            continue;
        CHECK_MSG(a.default_trigger.empty(), "the global action '{}' ships bound to '{}'", a.id,
                  a.default_trigger);
    }
    // And the application ones that operators expect to work out of the box, do.
    CHECK_EQ(def_of("edit.undo").default_trigger, std::string("Ctrl+Z"));
    CHECK_EQ(def_of("file.save").default_trigger, std::string("Ctrl+S"));
}

LTEST_CASE(ctrl_z_is_the_documents_and_the_workspace_has_its_own) {
    // ADR-0021: separating the stacks is what made this choice available. With one stack there is
    // no choice, only a gamble about which thing a keystroke reaches.
    CHECK_EQ(def_of("edit.undo").default_trigger, std::string("Ctrl+Z"));
    CHECK_EQ(def_of("workspace.undo").default_trigger, std::string("Ctrl+Alt+Z"));
    CHECK(def_of("edit.undo").scope == Scope::Application);
    CHECK(def_of("workspace.undo").scope == Scope::Application);
}

LTEST_CASE(scopes_are_assigned_by_what_the_action_does) {
    // The thing that decides is the action, not the binding. Ctrl+Z firing while the operator
    // types in a browser would be actively harmful; "stop streaming" only working while the
    // window is focused would be useless, because the whole point is that they are in the game.
    for (const std::string_view app : {"edit.undo", "edit.redo", "file.save", "layer.delete",
                                       "workspace.reset"})
        CHECK_MSG(def_of(app).scope == Scope::Application, "{} should be application-scoped",
                  app);
    for (const std::string_view global : {"output.toggle-stream", "output.toggle-record",
                                          "scene.switch", "safe.panic", "audio.push-to-talk"})
        CHECK_MSG(def_of(global).scope == Scope::Global, "{} should be global", global);
}

LTEST_CASE(an_action_reference_round_trips) {
    const ActionRef plain{"edit.undo", std::nullopt};
    CHECK_EQ(plain.to_string(), std::string("edit.undo"));
    CHECK(ActionRef::parse(plain.to_string()).value() == plain);

    const Id scene = Id::generate();
    const ActionRef with_arg{"scene.switch", scene};
    CHECK(with_arg.to_string().find("scene.switch:") == 0);
    const auto back = ActionRef::parse(with_arg.to_string());
    REQUIRE(back.has_value());
    CHECK(*back == with_arg);
    CHECK(back->argument.value() == scene);

    CHECK(!ActionRef::parse("scene.switch:not-an-id").has_value());
    CHECK(!ActionRef::parse("").has_value());
    CHECK(!ActionRef::parse(":01J0").has_value());
}

LTEST_CASE(switching_scene_resolves_to_a_command) {
    const ldoc::Project p = show();
    const Id target = p.scenes[1].id;

    const Resolved r = resolve(ActionRef{"scene.switch", target}, p);
    REQUIRE(r.outcome == Outcome::Command);
    REQUIRE(r.command.has_value());
    const auto* cmd = std::get_if<ldoc::SetDeckProgram>(r.command.operator->());
    REQUIRE(cmd != nullptr);
    CHECK(cmd->scene.value() == target);
    CHECK(cmd->deck == p.decks[0].id);
}

LTEST_CASE(the_panic_button_cuts_to_the_emergency_scene) {
    // ADR-0025. An ordinary command, so it is journaled and undoable -- which matters, because
    // the operator will want to get back exactly where they were.
    ldoc::Project p = show();
    const Id safe = p.scenes[1].id;

    // With none set, it says so rather than doing something arbitrary.
    const Resolved without = resolve(ActionRef{"safe.panic", std::nullopt}, p);
    CHECK(without.outcome == Outcome::Unknown);
    CHECK(without.note.find("no emergency scene") != std::string::npos);

    p.canvases[0].emergency_scene = safe;
    const Resolved with = resolve(ActionRef{"safe.panic", std::nullopt}, p);
    REQUIRE(with.outcome == Outcome::Command);
    const auto* cmd = std::get_if<ldoc::SetDeckProgram>(with.command.operator->());
    REQUIRE(cmd != nullptr);
    CHECK(cmd->scene.value() == safe);
}

LTEST_CASE(session_operations_resolve_without_a_command) {
    const ldoc::Project p = show();
    CHECK(resolve(ActionRef{"edit.undo", std::nullopt}, p).outcome == Outcome::Undo);
    CHECK(resolve(ActionRef{"edit.redo", std::nullopt}, p).outcome == Outcome::Redo);
    CHECK(resolve(ActionRef{"file.save", std::nullopt}, p).outcome == Outcome::Save);
    CHECK(!resolve(ActionRef{"edit.undo", std::nullopt}, p).command.has_value());
}

LTEST_CASE(an_action_with_nothing_behind_it_yet_says_so_rather_than_vanishing) {
    // It stays in the catalogue so the table is complete and a binding made today survives until
    // the feature lands, instead of the operator rebinding once it does.
    const ldoc::Project p = show();
    const Resolved r = resolve(ActionRef{"output.toggle-stream", std::nullopt}, p);
    CHECK(r.outcome == Outcome::NotYetAvailable);
    CHECK(r.note.find("not wired up yet") != std::string::npos);
    CHECK(find_action("output.toggle-stream") != nullptr);
}

LTEST_CASE(bad_references_are_reported_not_guessed_at) {
    const ldoc::Project p = show();
    CHECK(resolve(ActionRef{"no.such.action", std::nullopt}, p).outcome == Outcome::Unknown);
    // A parameterised action with nothing to act on.
    CHECK(resolve(ActionRef{"scene.switch", std::nullopt}, p).outcome == Outcome::Unknown);
    // And one pointed at a scene that has been deleted since the binding was made.
    const Resolved gone = resolve(ActionRef{"scene.switch", Id::generate()}, p);
    CHECK(gone.outcome == Outcome::Unknown);
    CHECK(gone.note.find("no longer exists") != std::string::npos);
}

LTEST_CASE(the_table_lists_one_row_per_scene_for_parameterised_actions) {
    // Exactly as OBS lists them, and modelling it as an argument rather than one action per
    // scene is what keeps the binding attached when a scene is renamed.
    const ldoc::Project p = show();
    const auto actions = available_actions(p);

    int switches = 0;
    for (const ActionRef& a : actions)
        switches += a.action == "scene.switch" ? 1 : 0;
    CHECK_EQ(switches, static_cast<int>(p.scenes.size()));

    CHECK_EQ(describe(ActionRef{"scene.switch", p.scenes[1].id}, p),
             std::string("Switch to Scene: Gameplay"));
    CHECK_EQ(describe(ActionRef{"edit.undo", std::nullopt}, p), std::string("Undo"));
    CHECK_EQ(describe(ActionRef{"scene.switch", Id::generate()}, p),
             std::string("Switch to Scene: (missing)"));
}

LTEST_CASE(bindings_start_at_the_defaults_and_remember_only_what_changed) {
    // So that a release which changes a default changes it for everyone who never touched that
    // binding, and leaves alone everyone who did.
    Bindings b;
    const ActionRef undo{"edit.undo", std::nullopt};
    CHECK_EQ(b.trigger_for(undo), std::string("Ctrl+Z"));
    CHECK(b.is_default(undo));
    CHECK_EQ(b.override_count(), std::size_t{0});

    b.set_trigger(undo, "Ctrl+U");
    CHECK_EQ(b.trigger_for(undo), std::string("Ctrl+U"));
    CHECK(!b.is_default(undo));
    CHECK_EQ(b.override_count(), std::size_t{1});

    b.reset(undo);
    CHECK_EQ(b.trigger_for(undo), std::string("Ctrl+Z"));
    CHECK(b.is_default(undo));
}

LTEST_CASE(unbinding_is_not_the_same_as_resetting) {
    // "I want no key on this" has to survive, or the next launch hands the default back.
    Bindings b;
    const ActionRef save{"file.save", std::nullopt};
    b.set_trigger(save, "");
    CHECK_EQ(b.trigger_for(save), std::string(""));
    CHECK(!b.is_default(save));

    const auto back = Bindings::from_toml(b.to_toml());
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK_MSG(back->trigger_for(save).empty(), "a deliberate unbinding came back as the default");

    b.reset_all();
    CHECK_EQ(b.trigger_for(save), std::string("Ctrl+S"));
}

LTEST_CASE(a_conflicting_trigger_is_reported_rather_than_silently_stolen) {
    const ldoc::Project p = show();
    Bindings b;
    const ActionRef undo{"edit.undo", std::nullopt};
    const ActionRef save{"file.save", std::nullopt};

    const auto clash = b.conflict(save, "Ctrl+Z", p);
    REQUIRE(clash.has_value());
    CHECK(*clash == undo);

    CHECK(!b.conflict(save, "Ctrl+Alt+Y", p).has_value());
    CHECK(!b.conflict(save, "", p).has_value());
    // An action never conflicts with itself.
    CHECK(!b.conflict(undo, "Ctrl+Z", p).has_value());
}

LTEST_CASE(an_application_shortcut_and_a_global_one_may_share_a_key) {
    // They are delivered by different things in different circumstances. Refusing it would make
    // the global list needlessly cramped for no benefit.
    const ldoc::Project p = show();
    Bindings b;
    b.set_trigger(ActionRef{"output.toggle-stream", std::nullopt}, "Ctrl+Z");
    CHECK_MSG(!b.conflict(ActionRef{"edit.undo", std::nullopt}, "Ctrl+Z", p).has_value(),
              "a global binding was treated as conflicting with an application one");

    // But two global ones do conflict with each other.
    b.set_trigger(ActionRef{"output.toggle-record", std::nullopt}, "F9");
    const auto clash =
        b.conflict(ActionRef{"output.save-replay", std::nullopt}, "F9", p);
    REQUIRE(clash.has_value());
    CHECK_EQ(clash->action, std::string("output.toggle-record"));
}

LTEST_CASE(bindings_round_trip_through_their_file) {
    const ldoc::Project p = show();
    Bindings b;
    b.set_trigger(ActionRef{"edit.undo", std::nullopt}, "Ctrl+U");
    b.set_trigger(ActionRef{"output.toggle-stream", std::nullopt}, "F10");
    b.set_trigger(ActionRef{"scene.switch", p.scenes[1].id}, "Ctrl+2");

    const std::string text = b.to_toml();
    const auto back = Bindings::from_toml(text);
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK_EQ(back->override_count(), std::size_t{3});
    CHECK_EQ(back->trigger_for(ActionRef{"edit.undo", std::nullopt}), std::string("Ctrl+U"));
    CHECK_EQ(back->trigger_for(ActionRef{"scene.switch", p.scenes[1].id}), std::string("Ctrl+2"));
    CHECK_EQ(back->to_toml(), text);

    // Untouched bindings are not in the file at all.
    CHECK(text.find("file.save") == std::string::npos);
}

LTEST_CASE(a_malformed_bindings_file_is_rejected_with_a_reason) {
    for (const std::string& bad : {
             std::string{"not toml"},
             std::string{"[bindings]\n\"edit.undo\" = \"Ctrl+Z\"\n"},   // no version
             std::string{"version = 99\n[bindings]\n"},
             std::string{"version = 1\n[bindings]\n\"edit.undo\" = 7\n"},
             std::string{"version = 1\n[bindings]\n\"\" = \"Ctrl+Z\"\n"},
         }) {
        const auto r = Bindings::from_toml(bad);
        CHECK_MSG(!r.has_value(), "accepted: {}", bad.substr(0, 40));
        if (!r.has_value())
            CHECK(!r.error().empty());
    }
    // An empty but well-formed file is simply "everything is at its default".
    const auto empty = Bindings::from_toml("version = 1\n");
    REQUIRE(empty.has_value());
    CHECK_EQ(empty->override_count(), std::size_t{0});
    CHECK_EQ(empty->trigger_for(ActionRef{"edit.undo", std::nullopt}), std::string("Ctrl+Z"));
}

LTEST_CASE(a_binding_for_an_action_this_build_removed_does_not_break_the_file) {
    // A downgrade, or a renamed action. The binding is kept rather than discarded, so upgrading
    // again finds it where the operator left it.
    const auto back = Bindings::from_toml(
        "version = 1\n[bindings]\n\"future.action\" = \"Ctrl+9\"\n");
    REQUIRE_MSG(back.has_value(), "{}", back.error());
    CHECK_EQ(back->override_count(), std::size_t{1});
    CHECK(back->to_toml().find("future.action") != std::string::npos);
    // It simply resolves to nothing.
    CHECK(resolve(ActionRef{"future.action", std::nullopt}, show()).outcome == Outcome::Unknown);
}

LTEST_MAIN("actions")
