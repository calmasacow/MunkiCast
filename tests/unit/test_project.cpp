// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ltest.hpp"

#include <ldoc/project.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace ldoc;

namespace {

/// A directory that cleans up after itself, so a failing case cannot leave a tree behind that
/// the next run then finds and trips over.
struct TempDir {
    fs::path path;

    TempDir() {
        path = fs::temp_directory_path() /
               ("munkicast-project-test-" + Id::generate().to_string());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// A collection bundle, as far as `project_collections` is concerned: a directory with a
/// project.json in it.
void make_collection(const fs::path& at) {
    fs::create_directories(at);
    std::ofstream{at / "project.json"} << "{}";
}

} // namespace

LTEST_CASE(a_new_project_gets_the_whole_tree_and_a_manifest) {
    TempDir tmp;
    const fs::path dir = tmp.path / "MyShow";

    const auto made = create_project_tree(dir, "My Show");
    REQUIRE_MSG(made.has_value(), "{}", made.error());
    CHECK_EQ(made->name, std::string{"My Show"});
    CHECK(!made->id.is_nil());

    // Every folder, because a project is a thing you hand to someone else: a tree missing
    // Exports is one where the export queue has nowhere to write.
    for (const std::string& folder : project_folders())
        CHECK_MSG(fs::is_directory(dir / folder), "missing {}", folder);

    CHECK(is_project(dir));
    // The manifest is named after the project, not a fixed project.munki: opening the folder
    // should show "MyShow.munki" rather than the same filename every project has.
    CHECK(fs::is_regular_file(dir / "MyShow.munki"));
    CHECK(!fs::exists(dir / "project.munki"));
}

LTEST_CASE(a_project_is_not_a_scene_collection) {
    // The distinction this whole file exists for. A project *contains* collections; it is not
    // one. A directory holding only a collection's project.json is not a project, and a
    // project directory is not itself readable as a collection.
    TempDir tmp;
    const fs::path collection = tmp.path / "just-a-collection";
    make_collection(collection);
    CHECK(!is_project(collection));

    const fs::path project = tmp.path / "Real Project";
    REQUIRE(create_project_tree(project, "Real Project").has_value());
    CHECK(is_project(project));
    CHECK(!fs::exists(project / "project.json"));
}

LTEST_CASE(a_project_lists_the_collections_inside_it) {
    TempDir tmp;
    const fs::path dir = tmp.path / "Season Two";
    REQUIRE(create_project_tree(dir, "Season Two").has_value());

    CHECK(project_collections(dir).empty());

    make_collection(collections_dir(dir) / "01AAAAAAAAAAAAAAAAAAAAAAAA.munkicast");
    make_collection(collections_dir(dir) / "01BBBBBBBBBBBBBBBBBBBBBBBB.munkicast");
    // Not a collection: no project.json, so it is someone else's folder and left alone.
    fs::create_directories(collections_dir(dir) / "notes");

    const auto found = project_collections(dir);
    REQUIRE(found.size() == std::size_t{2});
    CHECK_EQ(found[0].filename().string(),
             std::string{"01AAAAAAAAAAAAAAAAAAAAAAAA.munkicast"});
    CHECK_EQ(found[1].filename().string(),
             std::string{"01BBBBBBBBBBBBBBBBBBBBBBBB.munkicast"});
}

LTEST_CASE(a_manifest_round_trips) {
    TempDir tmp;
    const fs::path dir = tmp.path / "Round Trip";
    const auto made = create_project_tree(dir, "Round Trip");
    REQUIRE(made.has_value());

    ProjectManifest changed = *made;
    changed.open_collection = "01CCCCCCCCCCCCCCCCCCCCCCCC.munkicast";
    REQUIRE(save_manifest(dir, changed).has_value());

    const auto read = load_manifest(dir);
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK_EQ(read->name, std::string{"Round Trip"});
    CHECK(read->id == made->id);
    CHECK_EQ(read->open_collection, changed.open_collection);
}

LTEST_CASE(making_a_project_where_one_already_is_is_refused) {
    TempDir tmp;
    const fs::path dir = tmp.path / "Taken";
    REQUIRE(create_project_tree(dir, "Taken").has_value());

    // Merging would make two projects share one Recordings folder, and nothing downstream
    // could then say whose a file was.
    const auto again = create_project_tree(dir, "Also Taken");
    CHECK(!again.has_value());

    // And the first project is untouched by the refusal.
    const auto read = load_manifest(dir);
    REQUIRE(read.has_value());
    CHECK_EQ(read->name, std::string{"Taken"});
}

LTEST_CASE(a_project_whose_folder_was_renamed_still_opens) {
    // The manifest is named after the folder, so renaming the folder in a file manager would
    // otherwise leave a project that is no longer one. The manifest is found by its
    // extension, and saving brings the name back in line.
    TempDir tmp;
    const fs::path made_at = tmp.path / "Before";
    const auto made = create_project_tree(made_at, "Before");
    REQUIRE_MSG(made.has_value(), "{}", made.error());

    const fs::path now_at = tmp.path / "After";
    fs::rename(made_at, now_at);

    CHECK(is_project(now_at));
    const auto read = load_manifest(now_at);
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK(read->id == made->id);

    // Saving writes it under the folder's current name.
    REQUIRE(save_manifest(now_at, *read).has_value());
    CHECK(fs::is_regular_file(now_at / "After.munki"));
}

LTEST_CASE(a_project_written_by_an_older_build_still_opens_and_is_brought_up_to_date) {
    // Before this, every project's manifest was called project.munki. Those are still
    // projects; saving gives them a name of their own and clears the old file, so the folder
    // does not end up carrying both.
    TempDir tmp;
    const fs::path dir = tmp.path / "Legacy";
    REQUIRE(create_project_tree(dir, "Legacy").has_value());

    // Put it back the way an older build left it.
    fs::rename(dir / "Legacy.munki", dir / "project.munki");
    REQUIRE(fs::exists(dir / "project.munki"));
    CHECK(is_project(dir));

    const auto read = load_manifest(dir);
    REQUIRE_MSG(read.has_value(), "{}", read.error());
    CHECK_EQ(read->name, std::string{"Legacy"});

    REQUIRE(save_manifest(dir, *read).has_value());
    CHECK(fs::is_regular_file(dir / "Legacy.munki"));
    CHECK(!fs::exists(dir / "project.munki"));
}

LTEST_CASE(a_project_will_not_be_built_inside_a_folder_that_already_has_files) {
    // The bug this exists for: "New Project" asked only for a location and built the tree
    // inside whatever was picked. Choosing an existing media folder scattered Collections,
    // Recordings and the rest through it, with nothing to undo it. A project owns its folder.
    TempDir tmp;
    const fs::path busy = tmp.path / "My Videos";
    fs::create_directories(busy);
    std::ofstream{busy / "holiday.mp4"} << "not really a video";

    const auto made = create_project_tree(busy, "Scattered");
    CHECK(!made.has_value());

    // Nothing was made, including the folders -- a half-built tree left behind would be the
    // same mess arriving by a different route.
    CHECK(!fs::exists(busy / "Collections"));
    CHECK(!fs::exists(busy / "Recordings"));
    CHECK(!is_project(busy));
    CHECK(fs::exists(busy / "holiday.mp4"));
}

LTEST_CASE(an_existing_empty_folder_is_a_fine_place_for_a_project) {
    // Making the folder first and then the project in it is a normal way to reach this, so
    // emptiness is the test rather than non-existence.
    TempDir tmp;
    const fs::path empty = tmp.path / "Ready And Waiting";
    fs::create_directories(empty);

    const auto made = create_project_tree(empty, "Ready And Waiting");
    REQUIRE_MSG(made.has_value(), "{}", made.error());
    CHECK(is_project(empty));
}

LTEST_CASE(a_project_from_a_newer_build_is_refused_rather_than_half_read) {
    TempDir tmp;
    const fs::path dir = tmp.path / "From The Future";
    REQUIRE(create_project_tree(dir, "From The Future").has_value());

    std::ofstream{manifest_path(dir)}
        << R"({"schema_version": 99, "id": "01DDDDDDDDDDDDDDDDDDDDDDDD", )"
        << R"("name": "From The Future", "open_collection": ""})";

    // Opening it anyway would mean writing it back with every field this build does not know
    // silently dropped, which is how a newer project gets quietly destroyed by an older one.
    const auto read = load_manifest(dir);
    CHECK(!read.has_value());
}

LTEST_CASE(opening_a_project_remakes_a_folder_someone_deleted) {
    TempDir tmp;
    const fs::path dir = tmp.path / "Patched Up";
    REQUIRE(create_project_tree(dir, "Patched Up").has_value());

    std::error_code ec;
    fs::remove_all(dir / "Exports", ec);
    REQUIRE(!fs::exists(dir / "Exports"));

    const auto problems = ensure_project_folders(dir);
    CHECK(problems.empty());
    CHECK(fs::is_directory(dir / "Exports"));
}

LTEST_MAIN("project")
