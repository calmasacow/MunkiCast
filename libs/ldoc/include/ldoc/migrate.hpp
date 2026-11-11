// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"
#include "ldoc/json.hpp"

#include <vector>

/// Forward-only schema migration.
///
/// Kept in its own file rather than inside the reader, because a migration is not parsing. The
/// reader's job is to turn bytes into whatever the file says; this is where a project that was
/// written by an older build is brought up to what this build means. Separating them is what
/// makes a migration testable from a project built in memory, with no file involved.
namespace ldoc {

/// What a migration did, so the operator can be told rather than left to notice.
struct Migration {
    /// The version the project arrived as. Equal to `kSchemaVersion` when nothing was done.
    int from = kSchemaVersion;
    /// Anything worth saying out loud -- dropped fields, invented objects. Never silent.
    std::vector<Problem> problems;

    [[nodiscard]] bool happened() const { return from != kSchemaVersion; }
};

/// Bring `project` up to `kSchemaVersion`, in place.
///
/// Call it after the scenes have been loaded into `project.scenes`, because version 1 scenes
/// have no canvas of their own and the only honest answer -- the first canvas, which is the only
/// one the shell ever worked against -- needs the canvas list to be there.
///
/// `legacy_outputs` comes from `ProjectFile`, and is what a version 1 output carried instead of
/// naming an encode. Empty is correct for a version 2 file and for a project built in memory.
///
/// A project already at `kSchemaVersion` is left exactly as it is. A project *newer* than this
/// build is not this function's problem: the reader refuses it and the caller opens read-only,
/// because migrating backwards would mean dropping the fields this build does not know about.
[[nodiscard]] Migration migrate(Project& project,
                                const std::vector<LegacyOutputEncode>& legacy_outputs = {});

} // namespace ldoc
