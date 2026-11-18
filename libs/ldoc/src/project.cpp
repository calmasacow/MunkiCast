// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/project.hpp"

#include "lcore/atomic_file.hpp"

#include <json/json.h>

#include <algorithm>
#include <format>
#include <memory>
#include <sstream>
#include <system_error>

namespace ldoc {
namespace {

namespace fs = std::filesystem;
// `lc` is the namespace lcore declares.

constexpr std::string_view kManifestFile = "project.munki";
constexpr std::string_view kCollectionsDir = "Collections";
constexpr int kProjectSchemaVersion = 1;

} // namespace

const std::vector<std::string>& project_folders() {
    static const std::vector<std::string> folders{
        "Collections", "Timelines", "Graphics", "Recordings",
        "SourceMedia", "CacheMedia", "Exports", "Saves",
    };
    return folders;
}

fs::path manifest_path(const fs::path& dir) { return dir / kManifestFile; }

fs::path collections_dir(const fs::path& dir) { return dir / kCollectionsDir; }

bool is_project(const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(manifest_path(dir), ec) && !ec;
}

std::vector<std::string> ensure_project_folders(const fs::path& dir) {
    std::vector<std::string> problems;
    for (const std::string& name : project_folders()) {
        std::error_code ec;
        fs::create_directories(dir / name, ec);
        if (ec) {
            problems.push_back(
                std::format("could not make the {} folder: {}", name, ec.message()));
        }
    }
    return problems;
}

std::expected<ProjectManifest, std::string> create_project_tree(const fs::path& dir,
                                                                const std::string& name) {
    if (dir.empty() || !dir.is_absolute())
        return std::unexpected("Choose an absolute path for the project.");
    if (name.empty())
        return std::unexpected("A project needs a name.");
    // Refused rather than merged into: two projects sharing a directory would share their
    // Recordings and Exports, and nothing downstream could tell whose a file was.
    if (is_project(dir))
        return std::unexpected(std::format("{} is already a project.", dir.string()));

    // And refused on any directory that already holds something. A project owns its folder:
    // building the tree inside a folder that was already somebody's -- their Videos, say --
    // scatters Collections and Recordings through it and there is no undo for that. An
    // existing *empty* directory is fine, because making the folder first and then the
    // project in it is a normal way to reach this.
    std::error_code existing;
    if (fs::is_directory(dir, existing) && !existing &&
        fs::directory_iterator(dir, existing) != fs::directory_iterator{} && !existing) {
        return std::unexpected(
            std::format("{} already has files in it. A project needs a folder of its own.",
                        dir.string()));
    }

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec)
        return std::unexpected(std::format("could not make {}: {}", dir.string(), ec.message()));

    static_cast<void>(ensure_project_folders(dir));

    ProjectManifest manifest;
    manifest.id = Id::generate();
    manifest.name = name;
    manifest.schema_version = kProjectSchemaVersion;
    if (const auto written = save_manifest(dir, manifest); !written)
        return std::unexpected(written.error());
    return manifest;
}

std::expected<ProjectManifest, std::string> load_manifest(const fs::path& dir) {
    const auto text = lc::read_file(manifest_path(dir));
    if (!text)
        return std::unexpected(text.error());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in{*text};
    if (!Json::parseFromStream(builder, in, &root, &errors))
        return std::unexpected(std::format("{} is not readable: {}", kManifestFile, errors));
    if (!root.isObject())
        return std::unexpected(std::format("{} is not an object", kManifestFile));

    const int version = root.get("schema_version", 0).asInt();
    if (version > kProjectSchemaVersion) {
        return std::unexpected(
            std::format("this project was written by a newer build (version {}; this build "
                        "understands {})",
                        version, kProjectSchemaVersion));
    }

    ProjectManifest manifest;
    manifest.schema_version = version;
    manifest.name = root.get("name", "").asString();
    manifest.open_collection = root.get("open_collection", "").asString();
    if (const auto id = Id::parse(root.get("id", "").asString()))
        manifest.id = *id;
    else
        return std::unexpected(std::format("{} has no usable id", kManifestFile));
    if (manifest.name.empty())
        return std::unexpected(std::format("{} has no name", kManifestFile));
    return manifest;
}

std::expected<void, std::string> save_manifest(const fs::path& dir,
                                               const ProjectManifest& manifest) {
    Json::Value root{Json::objectValue};
    root["schema_version"] = kProjectSchemaVersion;
    root["id"] = manifest.id.to_string();
    root["name"] = manifest.name;
    root["open_collection"] = manifest.open_collection;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return lc::write_atomic(manifest_path(dir), Json::writeString(builder, root));
}

std::vector<fs::path> project_collections(const fs::path& dir) {
    std::vector<fs::path> found;
    std::error_code ec;
    const fs::path root = collections_dir(dir);
    if (!fs::is_directory(root, ec) || ec)
        return found;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        // A collection is a directory holding a project.json. Anything else in here is not
        // ours and is left alone rather than reported: the operator's folder, their rules.
        if (!entry.is_directory())
            continue;
        std::error_code inner;
        if (fs::is_regular_file(entry.path() / "project.json", inner) && !inner)
            found.push_back(entry.path());
    }
    // Oldest first, which for ULID-named bundles is creation order.
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace ldoc
