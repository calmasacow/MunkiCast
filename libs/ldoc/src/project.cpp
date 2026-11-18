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

constexpr std::string_view kManifestExt = ".munki";
/// What the manifest was called before it was named after its project. Still opened, so a
/// project made by an earlier build does not stop being one.
constexpr std::string_view kLegacyManifestFile = "project.munki";
constexpr std::string_view kCollectionsDir = "Collections";
constexpr int kProjectSchemaVersion = 1;

} // namespace

const std::vector<std::string>& system_bin_folders() {
    // The three a new project starts with. Each maps straight onto a folder, so dropping a
    // file into one of them from a file manager puts it in the pool with no import step --
    // which is the point of their being system bins rather than ordinary ones.
    static const std::vector<std::string> folders{"Clips", "Recordings", "Graphics"};
    return folders;
}

std::vector<Bin> project_bins(const ProjectManifest& manifest) {
    std::vector<Bin> out;
    for (const std::string& folder : system_bin_folders()) {
        Bin bin;
        // Derived from the folder name, not stored: a system bin has no state of its own, so
        // there is nothing for a manifest to hold that the tree does not already say.
        bin.name = folder;
        bin.folder = folder;
        out.push_back(std::move(bin));
    }
    out.insert(out.end(), manifest.bins.begin(), manifest.bins.end());
    return out;
}

const std::vector<std::string>& project_folders() {
    static const std::vector<std::string> folders{
        "Collections", "Timelines", "Graphics",   "Recordings", "Clips",
        "SourceMedia", "CacheMedia", "Exports",   "Saves",
    };
    return folders;
}

fs::path manifest_path(const fs::path& dir) {
    // Named after the folder, which is named after the project: opening the folder, the
    // operator sees "Marathon.munki" rather than a row of identical project.munki files in
    // every project they have.
    return dir / (dir.filename().string() + std::string{kManifestExt});
}

fs::path find_manifest(const fs::path& dir) {
    const fs::path expected = manifest_path(dir);
    std::error_code ec;
    if (fs::is_regular_file(expected, ec) && !ec)
        return expected;

    // The folder may have been renamed since it was made, or written by a build that called
    // this project.munki. Take any .munki that is here -- the lowest name, so that a folder
    // with more than one opens the same way every time rather than by directory order.
    fs::path found;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file() || entry.path().extension() != kManifestExt)
            continue;
        if (found.empty() || entry.path().filename() < found.filename())
            found = entry.path();
    }
    if (!found.empty())
        return found;

    const fs::path legacy = dir / kLegacyManifestFile;
    return fs::is_regular_file(legacy, ec) && !ec ? legacy : fs::path{};
}

fs::path collections_dir(const fs::path& dir) { return dir / kCollectionsDir; }

bool is_project(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec)
        return false;
    return !find_manifest(dir).empty();
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
    const fs::path at = find_manifest(dir);
    if (at.empty())
        return std::unexpected(std::format("{} holds no project manifest", dir.string()));
    const auto text = lc::read_file(at);
    if (!text)
        return std::unexpected(text.error());

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream in{*text};
    if (!Json::parseFromStream(builder, in, &root, &errors))
        return std::unexpected(
            std::format("{} is not readable: {}", at.filename().string(), errors));
    if (!root.isObject())
        return std::unexpected(std::format("{} is not an object", at.filename().string()));

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
    for (const Json::Value& entry : root["bins"]) {
        if (!entry.isObject())
            continue;
        Bin bin;
        bin.name = entry.get("name", "").asString();
        if (bin.name.empty())
            continue;
        if (const auto id = Id::parse(entry.get("id", "").asString()))
            bin.id = *id;
        else
            bin.id = Id::generate();
        for (const Json::Value& item : entry["items"]) {
            if (item.isString() && !item.asString().empty())
                bin.items.push_back(item.asString());
        }
        manifest.bins.push_back(std::move(bin));
    }
    manifest.open_collection = root.get("open_collection", "").asString();
    if (const auto id = Id::parse(root.get("id", "").asString()))
        manifest.id = *id;
    else
        return std::unexpected(std::format("{} has no usable id", at.filename().string()));
    if (manifest.name.empty())
        return std::unexpected(std::format("{} has no name", at.filename().string()));
    return manifest;
}

std::expected<void, std::string> save_manifest(const fs::path& dir,
                                               const ProjectManifest& manifest) {
    Json::Value root{Json::objectValue};
    root["schema_version"] = kProjectSchemaVersion;
    root["id"] = manifest.id.to_string();
    root["name"] = manifest.name;
    root["open_collection"] = manifest.open_collection;
    Json::Value bins{Json::arrayValue};
    for (const Bin& bin : manifest.bins) {
        Json::Value entry{Json::objectValue};
        entry["id"] = bin.id.to_string();
        entry["name"] = bin.name;
        Json::Value items{Json::arrayValue};
        for (const std::string& path : bin.items)
            items.append(path);
        entry["items"] = items;
        bins.append(entry);
    }
    root["bins"] = bins;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    const fs::path at = manifest_path(dir);
    if (const auto written = lc::write_atomic(at, Json::writeString(builder, root)); !written)
        return written;

    // Clear away the old fixed-name manifest once this project has one of its own, so the
    // folder does not carry both. Only that name: a stray .munki someone else put here is
    // not ours to delete.
    std::error_code ec;
    const fs::path legacy = dir / kLegacyManifestFile;
    if (legacy != at && fs::is_regular_file(legacy, ec) && !ec)
        fs::remove(legacy, ec);
    return {};
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
