// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The session boundary: everything that edits a document goes through this, and nothing outside
// it holds a mutable Project.

#include "lsession/local_session.hpp"

#include "ltest.hpp"
#include <lcore/atomic_file.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace lsession;
using ldoc::Command;
using ldoc::Id;

namespace {

class ScratchDir {
public:
    explicit ScratchDir(std::string_view tag) {
        path_ = fs::temp_directory_path() / std::format("libertas-sess-{}-{}", ::getpid(), tag);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] fs::path project() const { return path_ / "Show.libertas"; }

private:
    fs::path path_;
};

ldoc::Project starter() {
    ldoc::Project p = ldoc::make_default_project("Show");
    p.scenes[0].name = "Intro";
    p.scenes[0].roots.push_back(ldoc::make_node(ldoc::NodeKind::Source, "Camera"));
    return p;
}

/// Records what a listener was told, which is the only thing a caller is allowed to update from.
struct Recorder {
    std::vector<Change> changes;

    ChangeListener listener() {
        return [this](const Change& c) { changes.push_back(c); };
    }
    [[nodiscard]] const Change& last() const { return changes.back(); }
};

} // namespace

LTEST_CASE(an_in_memory_session_can_be_edited_without_a_home) {
    auto s = LocalSession::in_memory(starter());
    REQUIRE(s != nullptr);
    CHECK(s->path().empty());
    CHECK(!s->dirty());
    CHECK_EQ(s->revision(), std::uint64_t{0});

    const Id scene = s->project().scenes[0].id;
    const auto r = s->run(ldoc::RenameScene{scene, "Live"});
    REQUIRE_MSG(r.has_value(), "{}", r.error());
    CHECK_EQ(*r, std::string("Rename scene to 'Live'"));
    CHECK_EQ(s->project().scenes[0].name, std::string("Live"));
    CHECK(s->dirty());
    CHECK_EQ(s->revision(), std::uint64_t{1});

    // Nowhere to save to yet, and it says so rather than inventing a location.
    const auto saved = s->save();
    CHECK(!saved.has_value());
    CHECK(saved.error().find("location") != std::string::npos);
}

LTEST_CASE(missing_media_reports_source_and_path_and_clears_when_restored) {
    ScratchDir scratch{"missing-media"};
    const auto file = scratch.project();
    auto project = starter();
    for (const auto& [type, key] : {std::pair{"source.media", "localFile"},
                                    std::pair{"source.image", "file"},
                                    std::pair{"source.playlist", "files"},
                                    std::pair{"source.slideshow", "files"}}) {
        ldoc::Source source;
        source.id = Id::generate();
        source.type = type;
        source.name = "Missing clip";
        source.settings[key] = file.string();
        project.sources = {source};
        auto session = LocalSession::in_memory(project);
        const auto problems = session->problems();
        CHECK(std::ranges::any_of(problems, [&](const auto& problem) {
            return problem.severity == ldoc::Severity::Error && problem.subject == source.id &&
                   problem.message.find(source.name) != std::string::npos &&
                   problem.message.find(file.string()) != std::string::npos;
        }));
        REQUIRE(lc::write_atomic(file, "media placeholder"));
        CHECK(session->problems().empty());
        fs::remove(file);
    }
}

LTEST_CASE(listeners_are_told_what_was_applied) {
    // The rule the whole boundary rests on: a caller updates from the notification, never from
    // the return value of run(). So the notification has to carry enough to update from.
    auto s = LocalSession::in_memory(starter());
    Recorder rec;
    const int token = s->subscribe(rec.listener());

    const Id scene = s->project().scenes[0].id;
    const Id node = s->project().scenes[0].roots[0].id;
    const Command hide = ldoc::SetNodeVisible{scene, node, false};
    REQUIRE(s->run(hide).has_value());

    REQUIRE(rec.changes.size() == 1);
    CHECK(rec.last().kind == Change::Kind::Applied);
    REQUIRE(rec.last().command.has_value());
    CHECK_MSG(*rec.last().command == hide, "the listener was told about a different command");
    CHECK_EQ(rec.last().revision, std::uint64_t{1});

    // Undo reports the command that was *actually applied*, which is the inverse -- what a
    // replica of this document in another process would need in order to follow along.
    REQUIRE(s->undo().has_value());
    REQUIRE(rec.changes.size() == 2);
    CHECK(rec.last().kind == Change::Kind::Undone);
    REQUIRE(rec.last().command.has_value());
    const Command show = ldoc::SetNodeVisible{scene, node, true};
    CHECK(*rec.last().command == show);

    REQUIRE(s->redo().has_value());
    CHECK(rec.last().kind == Change::Kind::Redone);
    CHECK(*rec.last().command == hide);

    s->unsubscribe(token);
    REQUIRE(s->run(ldoc::RenameScene{scene, "quiet"}).has_value());
    CHECK_MSG(rec.changes.size() == 3, "a listener kept hearing after unsubscribing");
}

LTEST_CASE(a_refused_command_notifies_nobody_and_changes_nothing) {
    auto s = LocalSession::in_memory(starter());
    Recorder rec;
    s->subscribe(rec.listener());

    const auto r = s->run(ldoc::RenameScene{Id::generate(), "nowhere"});
    CHECK(!r.has_value());
    CHECK(rec.changes.empty());
    CHECK(!s->dirty());
    CHECK_EQ(s->revision(), std::uint64_t{0});
}

LTEST_CASE(undo_and_redo_report_when_there_is_nothing_to_do) {
    auto s = LocalSession::in_memory(starter());
    CHECK(!s->can_undo());
    CHECK(!s->can_redo());
    CHECK(!s->undo().has_value());
    CHECK(!s->redo().has_value());
    CHECK(s->undo_description().empty());

    const Id scene = s->project().scenes[0].id;
    REQUIRE(s->run(ldoc::RenameScene{scene, "Live"}).has_value());
    CHECK(s->can_undo());
    CHECK_EQ(s->undo_description(), std::string("Rename scene to 'Live'"));
}

LTEST_CASE(a_project_round_trips_through_create_and_open) {
    const ScratchDir sd{"roundtrip"};
    Id scene;
    {
        auto s = LocalSession::create(starter(), sd.project());
        REQUIRE_MSG(s.has_value(), "{}", s.error());
        scene = (*s)->project().scenes[0].id;
        CHECK_EQ((*s)->path(), sd.project());
        CHECK(!(*s)->dirty());
        REQUIRE((*s)->run(ldoc::RenameScene{scene, "Live"}).has_value());
        CHECK((*s)->dirty());
        REQUIRE_MSG((*s)->save().has_value(), "save failed");
        CHECK(!(*s)->dirty());
    }

    auto reopened = LocalSession::open(sd.project());
    REQUIRE_MSG(reopened.has_value(), "{}", reopened.error());
    CHECK_EQ((*reopened)->project().scenes[0].name, std::string("Live"));
    CHECK(!(*reopened)->dirty());
    CHECK(!(*reopened)->can_undo());
}

LTEST_CASE(saving_does_not_throw_away_the_undo_history) {
    // Saving is not something the operator did to the document. Clearing the stack here would
    // mean every save silently makes the last hour unundoable.
    const ScratchDir sd{"undo-across-save"};
    auto s = LocalSession::create(starter(), sd.project());
    REQUIRE(s.has_value());
    const Id scene = (*s)->project().scenes[0].id;

    REQUIRE((*s)->run(ldoc::RenameScene{scene, "One"}).has_value());
    REQUIRE((*s)->run(ldoc::RenameScene{scene, "Two"}).has_value());
    REQUIRE((*s)->save().has_value());

    CHECK_MSG((*s)->can_undo(), "the undo stack was lost on save");
    CHECK_EQ((*s)->undo_description(), std::string("Rename scene to 'Two'"));
    REQUIRE((*s)->undo().has_value());
    CHECK_EQ((*s)->project().scenes[0].name, std::string("One"));
    REQUIRE((*s)->undo().has_value());
    CHECK_EQ((*s)->project().scenes[0].name, std::string("Intro"));

    // And the generation carried forward, so the next save is a new one rather than a repeat.
    const std::uint64_t before = (*s)->project().generation;
    REQUIRE((*s)->save().has_value());
    CHECK(( *s)->project().generation > before);
}

LTEST_CASE(unsaved_work_survives_the_session_disappearing) {
    const ScratchDir sd{"recover"};
    Id scene;
    {
        auto s = LocalSession::create(starter(), sd.project());
        REQUIRE(s.has_value());
        scene = (*s)->project().scenes[0].id;
        REQUIRE((*s)->run(ldoc::RenameScene{scene, "Live"}).has_value());
        REQUIRE((*s)->run(ldoc::RenameNode{scene, (*s)->project().scenes[0].roots[0].id,
                                           "Cam A"}).has_value());
        // No save. The session goes out of scope exactly as it would if the process were killed.
    }

    auto reopened = LocalSession::open(sd.project());
    REQUIRE_MSG(reopened.has_value(), "{}", reopened.error());
    CHECK_EQ((*reopened)->project().scenes[0].name, std::string("Live"));
    CHECK_EQ((*reopened)->project().scenes[0].roots[0].name, std::string("Cam A"));
    CHECK_MSG((*reopened)->dirty(),
              "recovered work was reported as saved, so closing would lose it again");
}

LTEST_CASE(an_undo_survives_a_crash_too) {
    // The inverse is journaled like anything else, so a crash straight after an undo must not
    // bring the undone work back.
    const ScratchDir sd{"undo-recover"};
    Id scene;
    {
        auto s = LocalSession::create(starter(), sd.project());
        REQUIRE(s.has_value());
        scene = (*s)->project().scenes[0].id;
        REQUIRE((*s)->run(ldoc::RenameScene{scene, "Mistake"}).has_value());
        REQUIRE((*s)->undo().has_value());
        CHECK_EQ((*s)->project().scenes[0].name, std::string("Intro"));
    }

    auto reopened = LocalSession::open(sd.project());
    REQUIRE(reopened.has_value());
    CHECK_MSG((*reopened)->project().scenes[0].name == "Intro",
              "the undone change came back after a crash");
}

LTEST_CASE(a_project_cannot_be_open_in_two_sessions_at_once) {
    const ScratchDir sd{"lock"};
    auto first = LocalSession::create(starter(), sd.project());
    REQUIRE_MSG(first.has_value(), "{}", first.error());

    const auto second = LocalSession::open(sd.project());
    CHECK_MSG(!second.has_value(), "the project opened twice");
    if (!second.has_value())
        CHECK(second.error().find("already open") != std::string::npos);

    first->reset(); // closing it releases the lock
    const auto third = LocalSession::open(sd.project());
    CHECK_MSG(third.has_value(), "the lock outlived the session that held it");
}

LTEST_CASE(an_in_memory_project_can_be_given_a_home) {
    const ScratchDir sd{"saveas"};
    auto s = LocalSession::in_memory(starter());
    const Id scene = s->project().scenes[0].id;
    REQUIRE(s->run(ldoc::RenameScene{scene, "Live"}).has_value());

    REQUIRE_MSG(s->save_as(sd.project()).has_value(), "save_as failed");
    CHECK_EQ(s->path(), sd.project());
    CHECK(!s->dirty());

    // And it is a real project directory afterwards: editable, saveable, reopenable.
    REQUIRE(s->run(ldoc::RenameScene{scene, "Later"}).has_value());
    REQUIRE(s->save().has_value());
    CHECK(!s->save_as(sd.project()).has_value()); // it already has one

    auto reopened_after_close = [&] {
        s.reset();
        return LocalSession::open(sd.project());
    }();
    REQUIRE(reopened_after_close.has_value());
    CHECK_EQ((*reopened_after_close)->project().scenes[0].name, std::string("Later"));
}

LTEST_CASE(problems_combine_what_opening_found_with_what_the_document_says) {
    const ScratchDir sd{"problems"};
    auto s = LocalSession::create(starter(), sd.project());
    REQUIRE(s.has_value());
    CHECK((*s)->problems().empty());

    // A dangling deck pointer: legal, and reported.
    REQUIRE((*s)->run(ldoc::SetDeckProgram{(*s)->project().decks[0].id, Id::generate()})
                .has_value());
    const auto problems = (*s)->problems();
    CHECK_EQ(problems.size(), std::size_t{1});
    CHECK(problems.front().severity == ldoc::Severity::Warning);
}

LTEST_CASE(open_validation_refreshes_after_audio_is_ignored_or_routes_are_repaired) {
    const ScratchDir sd{"live-problems"};
    auto project = starter();
    ldoc::Source source;
    source.id = Id::generate();
    source.name = "Video";
    source.type = "source.media";
    const auto media = sd.project().parent_path() / "clip.mkv";
    REQUIRE(lc::write_atomic(media, "media placeholder"));
    source.settings["localFile"] = media.string();
    project.sources.push_back(source);
    auto& node = project.scenes.front().roots.front();
    auto& data = std::get<ldoc::SourceData>(node.data);
    data.source = source.id;
    data.audio.bus = Id::generate();
    const auto scene_id = project.scenes.front().id;
    const auto node_id = node.id;
    auto session = LocalSession::create(project, sd.project());
    REQUIRE(session.has_value());
    // Exactly one live warning, rather than an open-time copy plus the same live warning.
    CHECK_EQ((*session)->problems().size(), std::size_t{1});
    CHECK((*session)->problems().front().message.contains("Intro"));
    CHECK((*session)->problems().front().message.contains("Camera"));
    source.settings["ignoreAudio"] = true;
    REQUIRE((*session)->run(ldoc::SetSource{source}).has_value());
    CHECK((*session)->problems().empty());
    REQUIRE((*session)->undo().has_value());
    CHECK_EQ((*session)->problems().size(), std::size_t{1});
    auto audio = data.audio;
    audio.bus = project.buses.front().id;
    REQUIRE((*session)->run(ldoc::SetNodeAudio{scene_id, node_id, audio}).has_value());
    CHECK((*session)->problems().empty());
    REQUIRE((*session)->save().has_value());
    session->reset();
    session = LocalSession::open(sd.project());
    REQUIRE(session.has_value());
    CHECK((*session)->problems().empty());
}

LTEST_CASE(a_journal_that_cannot_be_written_is_a_warning_not_a_refusal_to_open) {
    if (::geteuid() == 0) {
        std::fprintf(stderr, "       (running as root; permissions cannot be tested)\n");
        return;
    }
    const ScratchDir sd{"nojournal"};
    {
        auto s = LocalSession::create(starter(), sd.project());
        REQUIRE(s.has_value());
    }
    const fs::path journal = ldoc::journal_path(sd.project());
    REQUIRE(fs::exists(journal));
    REQUIRE(::chmod(journal.c_str(), 0444) == 0);

    auto s = LocalSession::open(sd.project());
    REQUIRE_MSG(s.has_value(), "an unwritable journal stopped the project opening: {}",
                s.has_value() ? std::string{} : s.error());

    const auto problems = (*s)->problems();
    const bool warned = std::any_of(problems.begin(), problems.end(), [](const ldoc::Problem& p) {
        return p.message.find("recovery journal") != std::string::npos;
    });
    CHECK_MSG(warned, "the operator was not told their work is unprotected");

    // And editing still works; it is just not crash-proof.
    const Id scene = (*s)->project().scenes[0].id;
    CHECK((*s)->run(ldoc::RenameScene{scene, "Live"}).has_value());
    CHECK((*s)->save().has_value());

    ::chmod(journal.c_str(), 0644);
}

LTEST_CASE(saving_tells_listeners_without_claiming_the_document_changed) {
    const ScratchDir sd{"savednotify"};
    auto s = LocalSession::create(starter(), sd.project());
    REQUIRE(s.has_value());
    Recorder rec;
    (*s)->subscribe(rec.listener());

    REQUIRE((*s)->run(ldoc::RenameScene{(*s)->project().scenes[0].id, "Live"}).has_value());
    REQUIRE((*s)->save().has_value());

    REQUIRE(rec.changes.size() == 2);
    CHECK(rec.last().kind == Change::Kind::Saved);
    CHECK_MSG(!rec.last().command.has_value(), "a save reported a document change");
}

LTEST_CASE(a_session_refuses_newer_schema_without_rewriting_it) {
    ScratchDir scratch("future-schema");
    auto created = LocalSession::create(starter(), scratch.project());
    REQUIRE(created);
    created->reset();
    auto bytes = lc::read_file(scratch.project() / "project.json");
    REQUIRE(bytes);
    const auto key = std::format("\"schema_version\" : {}", ldoc::kSchemaVersion);
    const auto at = bytes->find(key);
    REQUIRE(at != std::string::npos);
    bytes->replace(at, key.size(), "\"schema_version\" : 99");
    REQUIRE(lc::write_atomic(scratch.project() / "project.json", *bytes));
    CHECK(!LocalSession::open(scratch.project()));
    CHECK_EQ(lc::read_file(scratch.project() / "project.json"), bytes);
}

LTEST_CASE(main_program_activation_fires_links_and_replays_exact_deck_states) {
    ScratchDir scratch{"scene-links"};
    auto p = starter();
    const Id main_scene = p.scenes.front().id;
    const Id main_deck = p.decks.front().id;
    std::vector<Id> targets;
    for (const auto* name : {"Vertical", "Square", "Independent"}) {
        ldoc::Canvas canvas;
        canvas.id = Id::generate();
        canvas.name = name;
        p.canvases.push_back(canvas);
        ldoc::Scene scene;
        scene.id = Id::generate();
        scene.name = name;
        scene.canvas = canvas.id;
        targets.push_back(scene.id);
        p.scenes.push_back(scene);
    }
    ldoc::Deck vertical;
    vertical.id = Id::generate();
    vertical.name = "Vertical";
    vertical.canvas = p.scenes[1].canvas;
    p.decks.push_back(vertical);
    p.scene_links.push_back({Id::generate(), main_scene, targets[0]});
    p.scene_links.push_back({Id::generate(), main_scene, targets[1]});
    // Stale imported references are ignored; even an imported chain never fires a second hop.
    p.scene_links.push_back({Id::generate(), main_scene, Id::generate()});
    p.scene_links.push_back({Id::generate(), targets[0], targets[2]});
    auto opened = LocalSession::create(p, scratch.project());
    REQUIRE(opened);
    auto session = std::move(*opened);
    const auto before = session->project();
    REQUIRE(session->run(ldoc::SetDeckProgram{main_deck, main_scene}));
    const auto after = session->project();
    REQUIRE(after.decks.size() == 3);
    CHECK(after.decks[1].program_scene == targets[0]);
    CHECK(after.decks[2].program_scene == targets[1]);
    CHECK(std::none_of(after.decks.begin(), after.decks.end(), [&](const auto& deck) {
        return deck.program_scene == targets[2];
    }));
    REQUIRE(session->undo());
    CHECK(session->project() == before);
    REQUIRE(session->redo());
    CHECK(session->project() == after);
    session.reset();
    opened = LocalSession::open(scratch.project());
    REQUIRE(opened);
    session = std::move(*opened);
    CHECK(session->project() == after);

    // Reasserting an already-live Main scene repairs a target changed independently.
    REQUIRE(session->run(ldoc::SetDeckProgram{vertical.id, std::nullopt}));
    REQUIRE(session->run(ldoc::SetDeckProgram{main_deck, main_scene}));
    CHECK(session->project().decks[1].program_scene == targets[0]);
    REQUIRE(session->run(ldoc::RemoveSceneLink{p.scene_links[0].id}));
    REQUIRE(session->run(ldoc::SetDeckProgram{vertical.id, std::nullopt}));
    REQUIRE(session->run(ldoc::SetDeckProgram{main_deck, main_scene}));
    CHECK(!session->project().decks[1].program_scene);

    // Older projects can also lack Main's deck; first activation still fires its links.
    p.decks.clear();
    auto fresh = LocalSession::in_memory(p);
    ldoc::Deck main = before.decks.front();
    REQUIRE(fresh->run(ldoc::AddDeck{main, 0}));
    REQUIRE(fresh->project().decks.size() == 3);
    CHECK(fresh->project().decks[1].program_scene == targets[0]);
    CHECK(fresh->project().decks[2].program_scene == targets[1]);
    REQUIRE(fresh->undo());
    CHECK(fresh->project() == p);
}

LTEST_MAIN("session")
