// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command journal: work done since the last save survives the process dying.

#include "ldoc/journal.hpp"

#include "ldoc/history.hpp"
#include "ldoc/project_io.hpp"
#include "lcore/atomic_file.hpp"
#include "ltest.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace ldoc;

namespace {

class ScratchDir {
public:
    explicit ScratchDir(std::string_view tag) {
        path_ = fs::temp_directory_path() / std::format("munkicast-jnl-{}-{}", ::getpid(), tag);
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

    [[nodiscard]] fs::path project() const { return path_ / "Show.munkicast"; }
    [[nodiscard]] fs::path file(std::string_view name) const { return path_ / name; }

private:
    fs::path path_;
};

Project starter() {
    Project p = make_default_project("Show");
    p.scenes[0].name = "Intro";
    p.scenes[0].roots.push_back(make_node(NodeKind::Source, "Camera"));
    return p;
}

std::string problems_of(const std::vector<Problem>& problems, Severity want) {
    std::string out;
    for (const Problem& p : problems) {
        if (p.severity != want)
            continue;
        if (!out.empty())
            out += "; ";
        out += p.message;
    }
    return out;
}

} // namespace

LTEST_CASE(commands_round_trip_through_the_journal) {
    const ScratchDir sd{"roundtrip"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    const Id scene = p.scenes[0].id;
    const Id node = p.scenes[0].roots[0].id;

    const std::vector<Command> written = {
        RenameScene{scene, "Live"},
        SetNodeVisible{scene, node, false},
        RenameNode{scene, node, "Cam A"},
    };
    {
        auto j = Journal::open(path);
        REQUIRE_MSG(j.has_value(), "{}", j.error());
        for (const Command& c : written)
            REQUIRE(j->record(c, 7).has_value());
    }

    const auto replay = Journal::read(path);
    REQUIRE_MSG(replay.has_value(), "{}", replay.error());
    CHECK(!replay->truncated_tail);
    CHECK_EQ(replay->unreadable, std::size_t{0});
    REQUIRE(replay->entries.size() == written.size());
    for (std::size_t i = 0; i < written.size(); ++i) {
        CHECK_MSG(replay->entries[i].command == written[i], "entry {} changed", i);
        CHECK_EQ(replay->entries[i].base_generation, std::uint64_t{7});
    }
}

LTEST_CASE(the_journal_is_one_line_per_command) {
    // Line-oriented on purpose: a document has to be closed to be valid, and a log that is only
    // valid once closed is useless for a crash.
    const ScratchDir sd{"lines"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    {
        auto j = Journal::open(path);
        REQUIRE(j.has_value());
        for (int i = 0; i < 5; ++i)
            REQUIRE(j->record(RenameScene{p.scenes[0].id, std::format("n{}", i)}, 0).has_value());
    }
    const auto text = lc::read_file(path);
    REQUIRE(text.has_value());
    CHECK_EQ(static_cast<std::size_t>(std::count(text->begin(), text->end(), '\n')),
             std::size_t{5});
    CHECK(text->back() == '\n');
}

LTEST_CASE(a_torn_final_line_costs_one_command_and_nothing_before_it) {
    // What a process dying mid-write looks like.
    const ScratchDir sd{"torn"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    const Id scene = p.scenes[0].id;
    {
        auto j = Journal::open(path);
        REQUIRE(j.has_value());
        REQUIRE(j->record(RenameScene{scene, "one"}, 1).has_value());
        REQUIRE(j->record(RenameScene{scene, "two"}, 1).has_value());
    }
    auto text = lc::read_file(path);
    REQUIRE(text.has_value());
    // Append half of a third entry, with no closing newline. Flattened first, because that is
    // the shape the journal writes: command_to_json is indented for reading.
    std::string partial = command_to_json(RenameScene{scene, "three"});
    partial.erase(std::remove(partial.begin(), partial.end(), '\n'), partial.end());
    REQUIRE(lc::write_atomic(path, *text + partial.substr(0, partial.size() / 2)).has_value());

    const auto replay = Journal::read(path);
    REQUIRE_MSG(replay.has_value(), "{}", replay.error());
    CHECK_MSG(replay->entries.size() == 2, "a torn tail cost {} entries",
              2 - replay->entries.size());
    CHECK_MSG(replay->truncated_tail, "the torn tail was not reported");
    CHECK_EQ(replay->unreadable, std::size_t{0});
}

LTEST_CASE(an_unreadable_line_in_the_middle_is_counted_not_silently_skipped) {
    const ScratchDir sd{"middle"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    const Id scene = p.scenes[0].id;

    std::string text = command_to_json(RenameScene{scene, "one"});
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    std::string content = text + "\n";
    content += "{ garbage in the middle\n";
    content += text + "\n";
    REQUIRE(lc::write_atomic(path, content).has_value());

    const auto replay = Journal::read(path);
    REQUIRE(replay.has_value());
    CHECK_EQ(replay->entries.size(), std::size_t{2});
    CHECK_EQ(replay->unreadable, std::size_t{1});
    CHECK(!replay->truncated_tail);
}

LTEST_CASE(a_name_containing_a_newline_still_round_trips) {
    // The journal is line-oriented and strips raw newlines before writing, which is only safe
    // because JSON escapes them inside strings. Worth proving rather than assuming.
    const ScratchDir sd{"newline"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    const Command c = RenameScene{p.scenes[0].id, "line one\nline two\ttabbed"};
    {
        auto j = Journal::open(path);
        REQUIRE(j.has_value());
        REQUIRE(j->record(c, 0).has_value());
    }
    const auto text = lc::read_file(path);
    REQUIRE(text.has_value());
    CHECK_EQ(static_cast<std::size_t>(std::count(text->begin(), text->end(), '\n')),
             std::size_t{1});

    const auto replay = Journal::read(path);
    REQUIRE(replay.has_value());
    REQUIRE(replay->entries.size() == 1);
    CHECK(replay->entries[0].command == c);
}

LTEST_CASE(clearing_the_journal_empties_it) {
    const ScratchDir sd{"clear"};
    const fs::path path = sd.file("journal.jsonl");
    const Project p = starter();
    auto j = Journal::open(path);
    REQUIRE(j.has_value());
    REQUIRE(j->record(RenameScene{p.scenes[0].id, "x"}, 0).has_value());
    REQUIRE(Journal::read(path)->entries.size() == 1);

    REQUIRE(j->clear().has_value());
    CHECK_EQ(Journal::read(path)->entries.size(), std::size_t{0});

    // And it is still usable afterwards.
    REQUIRE(j->record(RenameScene{p.scenes[0].id, "y"}, 0).has_value());
    CHECK_EQ(Journal::read(path)->entries.size(), std::size_t{1});
}

LTEST_CASE(reading_a_journal_that_was_never_written_is_not_an_error) {
    const ScratchDir sd{"absent"};
    const auto replay = Journal::read(sd.file("nothing.jsonl"));
    REQUIRE(replay.has_value());
    CHECK(replay->entries.empty());
}

LTEST_CASE(unsaved_work_survives_the_process_dying) {
    // The point of the whole mechanism: edits made after the last save are not lost because the
    // application never got to save them.
    const ScratchDir sd{"recover"};
    REQUIRE(create_project(starter(), sd.project()).has_value());

    Id scene;
    Id node;
    {
        auto opened = open_project(sd.project());
        REQUIRE_MSG(opened.has_value(), "{}", opened.error());
        DocumentHistory h{opened->project};
        scene = h.project().scenes[0].id;
        node = h.project().scenes[0].roots[0].id;

        auto j = Journal::open(journal_path(sd.project()));
        REQUIRE(j.has_value());
        const std::uint64_t base = h.project().generation;

        for (const Command& c : std::vector<Command>{RenameScene{scene, "Live"},
                                                     SetNodeVisible{scene, node, false},
                                                     RenameNode{scene, node, "Cam A"}}) {
            REQUIRE(h.run(c).has_value());
            REQUIRE(j->record(c, base).has_value());
        }
        // No save. The lock and the journal go out of scope exactly as they would if the
        // process had been killed here.
    }

    auto reopened = open_project(sd.project());
    REQUIRE_MSG(reopened.has_value(), "{}", reopened.error());
    CHECK_EQ(reopened->recovered_commands, std::size_t{3});
    CHECK_EQ(reopened->project.scenes[0].name, std::string("Live"));
    const Node* n = find_node(reopened->project.scenes[0].roots, node);
    REQUIRE(n != nullptr);
    CHECK(!n->visible);
    CHECK_EQ(n->name, std::string("Cam A"));
    CHECK(problems_of(reopened->problems, Severity::Error).empty());
    CHECK(problems_of(reopened->problems, Severity::Warning).find("recovered") !=
          std::string::npos);
}

LTEST_CASE(a_saved_journal_entry_is_never_applied_twice) {
    const ScratchDir sd{"twice"};
    REQUIRE(create_project(starter(), sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project p = std::move(opened->project);
    const Id scene = p.scenes[0].id;
    opened->lock.release();

    {
        auto j = Journal::open(journal_path(sd.project()));
        REQUIRE(j.has_value());
        const Command c = RenameScene{scene, "Live"};
        REQUIRE(apply(p, c).has_value());
        REQUIRE(j->record(c, p.generation).has_value());
    }
    REQUIRE(save_project(p, sd.project()).has_value());
    // save_project leaves the journal to its owner (the session clears or rotates it); the
    // entry is tagged with the old generation, so reopening must still not apply it twice.
    CHECK_EQ(Journal::read(journal_path(sd.project()))->entries.size(), std::size_t{1});

    auto reopened = open_project(sd.project());
    REQUIRE(reopened.has_value());
    CHECK_EQ(reopened->recovered_commands, std::size_t{0});
    CHECK_EQ(reopened->project.scenes[0].name, std::string("Live"));
}

LTEST_CASE(a_journal_left_from_an_older_generation_is_not_replayed) {
    // A crash *after* a save can leave entries behind that are already in the project. Replaying
    // them would apply the same edits a second time.
    const ScratchDir sd{"stale"};
    REQUIRE(create_project(starter(), sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project p = std::move(opened->project);
    const Id scene = p.scenes[0].id;
    const Id node = p.scenes[0].roots[0].id;
    opened->lock.release();

    // An entry recorded against generation 1, which a later save has already folded in.
    {
        auto j = Journal::open(journal_path(sd.project()));
        REQUIRE(j.has_value());
        REQUIRE(j->record(AddNode{scene, std::nullopt, 0,
                                  make_node(NodeKind::Source, "ghost")}, 1).has_value());
    }
    p.generation = 5; // pretend several saves have happened since
    REQUIRE(save_project(p, sd.project()).has_value());
    // Put the stale entry back, as a crash between the commit and the clear would.
    {
        auto j = Journal::open(journal_path(sd.project()));
        REQUIRE(j.has_value());
        REQUIRE(j->record(RenameNode{scene, node, "stale"}, 1).has_value());
    }

    auto reopened = open_project(sd.project());
    REQUIRE(reopened.has_value());
    CHECK_EQ(reopened->recovered_commands, std::size_t{0});
    CHECK_EQ(find_node(reopened->project.scenes[0].roots, node)->name, std::string("Camera"));
}

LTEST_CASE(a_journal_entry_that_cannot_be_replayed_is_reported) {
    // It should not happen -- the entry was applied once already -- so if it does, say so rather
    // than open a project that quietly disagrees with the log.
    const ScratchDir sd{"refused"};
    REQUIRE(create_project(starter(), sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    const std::uint64_t base = opened->project.generation;
    opened->lock.release();

    {
        auto j = Journal::open(journal_path(sd.project()));
        REQUIRE(j.has_value());
        REQUIRE(j->record(RenameScene{Id::generate(), "nowhere"}, base).has_value());
    }

    auto reopened = open_project(sd.project());
    REQUIRE(reopened.has_value());
    CHECK_EQ(reopened->recovered_commands, std::size_t{0});
    CHECK(problems_of(reopened->problems, Severity::Error).find("could not be replayed") !=
          std::string::npos);
}

LTEST_MAIN("journal")
