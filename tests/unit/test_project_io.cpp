// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A project on disk: the commit protocol, recovery from an interrupted save, and the promise
// that one bad scene costs one scene.

#include "ldoc/project_io.hpp"

#include "ldoc/json.hpp"
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
        path_ = fs::temp_directory_path() / std::format("munkicast-io-{}-{}", ::getpid(), tag);
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

private:
    fs::path path_;
};

Project two_scene_project() {
    Project p = make_default_project("Show");
    p.scenes[0].name = "Intro";
    p.scenes[0].roots.push_back(make_node(NodeKind::Source, "camera"));

    Scene second;
    second.id = Id::generate();
    second.name = "Gameplay";
    second.canvas = p.canvases[0].id;
    second.roots.push_back(make_node(NodeKind::Source, "capture"));
    p.scenes.push_back(std::move(second));
    return p;
}

std::vector<std::string> scene_names(const Project& p) {
    std::vector<std::string> names;
    for (const Scene& s : p.scenes)
        names.push_back(s.name);
    std::sort(names.begin(), names.end());
    return names;
}

std::string errors_of(const std::vector<Problem>& problems) {
    std::string out;
    for (const Problem& p : problems) {
        if (p.severity != Severity::Error)
            continue;
        if (!out.empty())
            out += "; ";
        out += p.message;
    }
    return out;
}

std::size_t count_files(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec)
            break;
        ++n;
    }
    return n;
}

} // namespace

LTEST_CASE(a_project_can_be_created_opened_and_saved) {
    const ScratchDir sd{"basic"};
    const Project original = two_scene_project();
    REQUIRE(create_project(original, sd.project()).has_value());

    CHECK(fs::exists(sd.project() / "project.json"));
    CHECK(fs::exists(sd.project() / "assets.json"));
    CHECK_EQ(count_files(sd.project() / "scenes"), std::size_t{2});

    auto opened = open_project(sd.project());
    REQUIRE_MSG(opened.has_value(), "{}", opened.error());
    CHECK_MSG(errors_of(opened->problems).empty(), "{}", errors_of(opened->problems));
    CHECK(!opened->read_only);
    CHECK(!opened->recovered);
    CHECK_EQ(opened->project.generation, std::uint64_t{1});
    CHECK_EQ(scene_names(opened->project),
             (std::vector<std::string>{"Gameplay", "Intro"}));

    // The model that comes back is the one that went in, generation aside.
    Project expected = original;
    expected.generation = opened->project.generation;
    CHECK_MSG(opened->project == expected, "the project changed on the way through the disk");
}

LTEST_CASE(a_successful_save_leaves_no_litter) {
    const ScratchDir sd{"litter"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project loaded = std::move(opened->project);
    opened->lock.release();

    loaded.scenes[0].name = "Intro v2";
    REQUIRE_MSG(save_project(loaded, sd.project()).has_value(), "save failed");
    CHECK_EQ(loaded.generation, std::uint64_t{2});

    // No staging directory survives a save that finished.
    std::error_code ec;
    for (auto it = fs::directory_iterator(sd.project() / ".munkicast", ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        CHECK_MSG(it->path().filename().string().find("staging") == std::string::npos,
                  "a staging directory was left behind: {}", it->path().string());
    }
    // And no half-installed temporaries.
    for (auto it = fs::directory_iterator(sd.project() / "scenes", ec);
         it != fs::directory_iterator(); it.increment(ec))
        CHECK(it->path().extension() == ".json");
}

LTEST_CASE(deleting_a_scene_removes_its_file_but_only_after_the_commit) {
    const ScratchDir sd{"delete"};
    Project p = two_scene_project();
    const Id gone = p.scenes[1].id;
    REQUIRE(create_project(p, sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project loaded = std::move(opened->project);
    opened->lock.release();

    loaded.scenes.erase(loaded.scenes.begin() + 1);
    REQUIRE(save_project(loaded, sd.project()).has_value());

    CHECK_EQ(count_files(sd.project() / "scenes"), std::size_t{1});
    CHECK(!fs::exists(sd.project() / "scenes" / std::format("{}.json", gone.to_string())));

    auto reopened = open_project(sd.project());
    REQUIRE(reopened.has_value());
    CHECK_EQ(reopened->project.scenes.size(), std::size_t{1});
    CHECK_MSG(errors_of(reopened->problems).empty(), "{}", errors_of(reopened->problems));
}

LTEST_CASE(a_save_interrupted_at_any_step_opens_to_one_generation_or_the_other) {
    // The requirement, stated plainly: interrupt a save at every commit step, then assert the
    // project opens to either the previous or the new generation -- never a mixture of the two.
    for (const SaveStep stop_after : {SaveStep::Staged, SaveStep::LeavesInstalled,
                                      SaveStep::RootInstalled, SaveStep::Done}) {
        const ScratchDir sd{std::format("torn-{}", static_cast<int>(stop_after))};
        Project p = two_scene_project();
        const Id intro = p.scenes[0].id;
        const Id gameplay = p.scenes[1].id;
        REQUIRE(create_project(p, sd.project()).has_value());

        auto opened = open_project(sd.project());
        REQUIRE(opened.has_value());
        Project loaded = std::move(opened->project);
        opened->lock.release();
        REQUIRE(loaded.generation == 1);

        // Generation 2: rename one scene, delete another, add a third.
        loaded.scenes[0].name = "Intro v2";
        loaded.scenes.erase(loaded.scenes.begin() + 1);
        Scene added;
        added.id = Id::generate();
        added.name = "Outro";
        added.canvas = loaded.canvases[0].id;
        const Id outro = added.id;
        loaded.scenes.push_back(std::move(added));

        Project attempt = loaded;
        const auto r = save_project(attempt, sd.project(), [stop_after](SaveStep step) {
            return step != stop_after; // stop *after* this step has happened
        });
        REQUIRE_MSG(r.has_value(), "step {}: {}", static_cast<int>(stop_after), r.error());

        auto reopened = open_project(sd.project());
        REQUIRE_MSG(reopened.has_value(), "step {}: {}", static_cast<int>(stop_after),
                    reopened.error());
        const Project& got = reopened->project;

        const bool is_old = got.generation == 1 &&
                            scene_names(got) == std::vector<std::string>{"Gameplay", "Intro"} &&
                            got.scene(intro) && got.scene(gameplay) && !got.scene(outro);
        const bool is_new = got.generation == 2 &&
                            scene_names(got) == std::vector<std::string>{"Intro v2", "Outro"} &&
                            got.scene(intro) && !got.scene(gameplay) && got.scene(outro);

        CHECK_MSG(is_old || is_new,
                  "interrupted after step {}: opened at generation {} with scenes {} -- a mixture",
                  static_cast<int>(stop_after), got.generation,
                  scene_names(got).empty() ? std::string{"none"} : scene_names(got).front());
        CHECK_MSG(errors_of(reopened->problems).empty(), "step {}: {}",
                  static_cast<int>(stop_after), errors_of(reopened->problems));

        // Whatever it opened as, it is internally consistent and can be saved again.
        Project again = got;
        CHECK_MSG(save_project(again, sd.project()).has_value(),
                  "step {}: the recovered project could not be saved",
                  static_cast<int>(stop_after));
    }
}

LTEST_CASE(an_interrupted_save_is_completed_on_open) {
    const ScratchDir sd{"rollforward"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project loaded = std::move(opened->project);
    opened->lock.release();

    loaded.scenes[0].name = "Changed";
    Project attempt = loaded;
    // Stop immediately after staging: nothing at all has been installed.
    REQUIRE(save_project(attempt, sd.project(), [](SaveStep s) {
                return s != SaveStep::Staged;
            }).has_value());
    CHECK_MSG(attempt.generation == 1, "an abandoned save reported a new generation");

    // On disk, project.json is still generation 1.
    const auto raw = lc::read_file(sd.project() / "project.json");
    REQUIRE(raw.has_value());
    REQUIRE(project_from_json(*raw)->project.generation == 1);

    auto reopened = open_project(sd.project());
    REQUIRE_MSG(reopened.has_value(), "{}", reopened.error());
    CHECK_MSG(reopened->recovered, "the interrupted save was not reported as recovered");
    CHECK_EQ(reopened->project.generation, std::uint64_t{2});
    CHECK_EQ(reopened->project.scenes[0].name, std::string("Changed"));
    CHECK(!reopened->problems.empty());
    CHECK(reopened->problems.front().severity == Severity::Warning);
}

LTEST_CASE(a_save_interrupted_while_writing_rolls_back_instead) {
    // Staging that never finished being written cannot be rolled forward, and must not be.
    const ScratchDir sd{"rollback"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project loaded = std::move(opened->project);
    opened->lock.release();

    loaded.scenes[0].name = "Never committed";
    Project attempt = loaded;
    REQUIRE(save_project(attempt, sd.project(), [](SaveStep s) {
                return s != SaveStep::Staged;
            }).has_value());

    // Simulate the interruption landing mid-write: one staged scene file never made it.
    std::error_code ec;
    const fs::path staging = sd.project() / ".munkicast" / "staging-2" / "scenes";
    bool removed = false;
    for (auto it = fs::directory_iterator(staging, ec); it != fs::directory_iterator();
         it.increment(ec)) {
        fs::remove(it->path(), ec);
        removed = true;
        break;
    }
    REQUIRE(removed);

    auto reopened = open_project(sd.project());
    REQUIRE_MSG(reopened.has_value(), "{}", reopened.error());
    CHECK_MSG(!reopened->recovered, "an incomplete save was rolled forward");
    CHECK_EQ(reopened->project.generation, std::uint64_t{1});
    CHECK_EQ(scene_names(reopened->project), (std::vector<std::string>{"Gameplay", "Intro"}));
    CHECK(!fs::exists(sd.project() / ".munkicast" / "staging-2"));
    CHECK_MSG(errors_of(reopened->problems).empty(), "{}", errors_of(reopened->problems));
}

LTEST_CASE(a_corrupt_scene_costs_one_scene_and_not_the_collection) {
    // The OBS failure this format exists to stop repeating.
    const ScratchDir sd{"quarantine"};
    Project p = two_scene_project();
    const Id broken = p.scenes[1].id;
    REQUIRE(create_project(p, sd.project()).has_value());

    const fs::path path = sd.project() / "scenes" / std::format("{}.json", broken.to_string());
    REQUIRE(lc::write_atomic(path, "{ this is not json").has_value());

    auto opened = open_project(sd.project());
    REQUIRE_MSG(opened.has_value(), "a corrupt scene stopped the project opening: {}",
                opened.error());
    CHECK_EQ(opened->project.scenes.size(), std::size_t{1});
    CHECK_EQ(opened->project.scenes[0].name, std::string("Intro"));

    // The bytes are not thrown away, and the message says where they went.
    const std::string errs = errors_of(opened->problems);
    CHECK(errs.find("Gameplay") != std::string::npos);
    CHECK(errs.find("quarantine") != std::string::npos);
    CHECK(!fs::exists(path));
    CHECK(fs::exists(sd.project() / ".munkicast" / "quarantine"));
    CHECK_EQ(count_files(sd.project() / ".munkicast" / "quarantine"), std::size_t{1});
}

LTEST_CASE(a_missing_scene_file_is_reported_rather_than_ignored) {
    const ScratchDir sd{"missing"};
    Project p = two_scene_project();
    const Id gone = p.scenes[1].id;
    REQUIRE(create_project(p, sd.project()).has_value());

    std::error_code ec;
    fs::remove(sd.project() / "scenes" / std::format("{}.json", gone.to_string()), ec);

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    CHECK_EQ(opened->project.scenes.size(), std::size_t{1});
    CHECK(errors_of(opened->problems).find("missing its file") != std::string::npos);
}

LTEST_CASE(a_scene_written_by_a_different_save_is_noticed) {
    const ScratchDir sd{"stale"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    // Rewrite one scene file stamped with a generation the project does not expect, which is what
    // a half-finished copy or a restored backup looks like.
    const fs::path path =
        sd.project() / "scenes" / std::format("{}.json", p.scenes[0].id.to_string());
    const auto text = lc::read_file(path);
    REQUIRE(text.has_value());
    const auto parsed = scene_from_json(*text);
    REQUIRE(parsed.has_value());
    REQUIRE(lc::write_atomic(path, scene_to_json(parsed->scene, 99)).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    CHECK_EQ(opened->project.scenes.size(), std::size_t{2});
    const bool warned = std::any_of(
        opened->problems.begin(), opened->problems.end(), [](const Problem& problem) {
            return problem.message.find("written by generation 99") != std::string::npos;
        });
    CHECK_MSG(warned, "a scene from a different generation loaded without comment");
}

LTEST_CASE(a_project_cannot_be_opened_twice_at_once) {
    // An OS-held lock, so it is released however the holder dies -- no stale pidfile to delete
    // by hand.
    const ScratchDir sd{"lock"};
    REQUIRE(create_project(two_scene_project(), sd.project()).has_value());

    auto first = open_project(sd.project());
    REQUIRE_MSG(first.has_value(), "{}", first.error());
    CHECK(first->lock.held());

    const auto second = open_project(sd.project());
    CHECK_MSG(!second.has_value(), "the project opened twice at once");
    if (!second.has_value())
        CHECK(second.error().find("already open") != std::string::npos);

    // Releasing it lets the next one in.
    first->lock.release();
    CHECK(!first->lock.held());
    const auto third = open_project(sd.project());
    CHECK_MSG(third.has_value(), "{}", third.has_value() ? std::string{} : third.error());
}

LTEST_CASE(a_project_from_a_newer_build_opens_read_only) {
    // Opening it, dropping the fields this build does not understand and saving it back is how a
    // downgrade silently destroys work.
    const ScratchDir sd{"newer"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    const auto text = lc::read_file(sd.project() / "project.json");
    REQUIRE(text.has_value());
    std::string bumped = *text;
    const std::string current = std::format("\"schema_version\" : {}", kSchemaVersion);
    const auto at = bumped.find(current);
    REQUIRE(at != std::string::npos);
    bumped.replace(at, current.size(), "\"schema_version\" : 99");
    REQUIRE(lc::write_atomic(sd.project() / "project.json", bumped).has_value());

    auto opened = open_project(sd.project());
    REQUIRE_MSG(opened.has_value(), "{}", opened.error());
    CHECK_MSG(opened->read_only, "a newer project opened writable");
    CHECK(errors_of(opened->problems).find("newer than this build") != std::string::npos);
    CHECK(opened->project.scenes.empty());
}

LTEST_CASE(opening_something_that_is_not_a_project_fails_clearly) {
    const ScratchDir sd{"notaproject"};
    CHECK(!open_project(sd.project()).has_value());

    std::error_code ec;
    fs::create_directories(sd.project(), ec);
    const auto r = open_project(sd.project());
    CHECK_MSG(!r.has_value(), "an empty directory opened as a project");
    CHECK(!r.error().empty());
}

LTEST_CASE(repeated_saves_advance_the_generation_and_stay_readable) {
    const ScratchDir sd{"repeat"};
    Project p = two_scene_project();
    REQUIRE(create_project(p, sd.project()).has_value());

    auto opened = open_project(sd.project());
    REQUIRE(opened.has_value());
    Project loaded = std::move(opened->project);
    opened->lock.release();

    for (int i = 0; i < 12; ++i) {
        loaded.scenes[0].name = std::format("Intro {}", i);
        REQUIRE_MSG(save_project(loaded, sd.project()).has_value(), "save {} failed", i);
        CHECK_EQ(loaded.generation, static_cast<std::uint64_t>(i + 2));

        auto check = open_project(sd.project());
        REQUIRE_MSG(check.has_value(), "open after save {}: {}", i, check.error());
        CHECK_EQ(check->project.scenes[0].name, std::format("Intro {}", i));
        CHECK_MSG(errors_of(check->problems).empty(), "save {}: {}", i,
                  errors_of(check->problems));
        check->lock.release();
    }
    CHECK_EQ(count_files(sd.project() / "scenes"), std::size_t{2});
}

LTEST_MAIN("project-io")
