// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "ldoc/types.hpp"

namespace ldoc {

/// A project: everything one piece of work is made of.
///
/// This is the outer construct, and it is *not* a scene collection. A collection is one
/// arrangement of scenes, sources and outputs -- one show. A project holds the collections
/// *and* everything else that work needs: the recordings it made, the timelines cut from
/// them, the graphics authored for it, the media cached for playback, and the settings that
/// were in force.
///
/// On disk:
///
///     MyProject/                  <- the project directory
///       MyProject.munki           <- this manifest: name, id, which collection is open
///       Collections/              <- the shows, each its own `.munkicast` bundle
///         <ulid>.munkicast/
///       Timelines/                <- .mtime files
///       Graphics/                 <- .manim files
///       Recordings/               <- what the recorder wrote
///       Clips/                    <- shorter pieces cut out of them
///       SourceMedia/              <- footage brought in
///       CacheMedia/               <- prepared copies, rebuildable
///       Exports/                  <- what the export queue wrote
///       Saves/                    <- snapshots the operator keeps
///
/// The Maya shape, and for Maya's reason: a project is a folder you can copy to another
/// machine, hand to someone else, or back up, and have the work arrive whole.
/// A bin in the media pool: a place media is organised into.
///
/// Two kinds, and the difference is where the contents come from. A *system* bin is a window
/// onto one of the project's own folders -- Clips, Recordings, Graphics -- so its contents are
/// whatever is in that folder, and a file copied in there by any means at all appears without
/// being imported. A bin the operator makes holds references to files wherever they are, which
/// is what makes it an arrangement rather than a second copy of the disk.
struct Bin {
    Id id;
    std::string name;
    /// The project folder this bin shows, for a system bin. Empty for one the operator made.
    std::string folder;
    /// Files in this bin, for one the operator made. Absolute paths: a bin can reference media
    /// that was never copied into the project.
    std::vector<std::string> items;

    [[nodiscard]] bool system() const { return !folder.empty(); }

    friend bool operator==(const Bin&, const Bin&) = default;
};

/// The folders a new project's system bins show, in the order they appear.
[[nodiscard]] const std::vector<std::string>& system_bin_folders();

struct ProjectManifest {
    Id id;
    std::string name;
    /// The collection that was open, relative to `Collections/`. Empty for a project whose
    /// collections have all been removed, which is a project that can still be opened.
    std::string open_collection;
    /// The bins the operator has made. The system bins are not here: they are the project's
    /// own folders, so storing them would be storing a fact the tree already states, and one
    /// that could then disagree with it.
    std::vector<Bin> bins;
    /// Written by whichever build last saved, so a newer project can say so rather than being
    /// read with fields silently dropped.
    int schema_version = 1;
};

/// The folders every project directory carries, in the order they are created.
///
/// Named as the operator sees them in a file manager, because they will. The model
/// directories inside a collection bundle are lower case and are not these: those belong to
/// the writer, and nothing outside it should be putting files in them.
[[nodiscard]] const std::vector<std::string>& project_folders();

/// Where this project's manifest goes: `<folder name>.munki` inside the folder.
///
/// Named after the project rather than a fixed `project.munki`, so that opening the folder
/// shows "Marathon.munki" instead of the same filename every project has.
[[nodiscard]] std::filesystem::path manifest_path(const std::filesystem::path& dir);

/// The manifest this folder actually has, which may not be at `manifest_path`: the folder
/// could have been renamed, or written by a build that used a fixed name. Empty when there
/// is none, which is what makes the folder not a project.
[[nodiscard]] std::filesystem::path find_manifest(const std::filesystem::path& dir);

/// Where this project's collections live.
[[nodiscard]] std::filesystem::path collections_dir(const std::filesystem::path& dir);

/// Whether this directory looks like a project: it has a manifest.
[[nodiscard]] bool is_project(const std::filesystem::path& dir);

/// Make a project directory: the tree, and a manifest naming it.
///
/// Refuses a directory that already holds a project rather than merging into it. An existing
/// empty directory is fine -- choosing a folder and then making a project in it is the normal
/// way this is reached.
[[nodiscard]] std::expected<ProjectManifest, std::string> create_project_tree(
    const std::filesystem::path& dir, const std::string& name);

/// Read a project's manifest.
[[nodiscard]] std::expected<ProjectManifest, std::string> load_manifest(
    const std::filesystem::path& dir);

/// Write a project's manifest, atomically.
[[nodiscard]] std::expected<void, std::string> save_manifest(const std::filesystem::path& dir,
                                                             const ProjectManifest& manifest);

/// The collections in a project, as absolute paths, oldest first.
[[nodiscard]] std::vector<std::filesystem::path> project_collections(
    const std::filesystem::path& dir);

/// Every bin in a project: the system bins for its folders, then the operator's own.
[[nodiscard]] std::vector<Bin> project_bins(const ProjectManifest& manifest);

/// Create any project folder that is missing.
///
/// Called on create and on open, so a project made by an older build -- or one whose folders
/// were moved or deleted between sessions -- gains them by being opened. A folder that cannot
/// be made is reported, not fatal: a read-only project is still worth reading.
[[nodiscard]] std::vector<std::string> ensure_project_folders(const std::filesystem::path& dir);

} // namespace ldoc
