// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

namespace munkicast {

/// Describe a project directory: what is in it, what is wrong with it, what it would do if the
/// show could not be supervised.
///
/// Reads the files directly rather than going through `open_project`, deliberately. A diagnostic
/// has to work on a project that is **currently open** -- which is exactly when someone is trying
/// to work out why it is misbehaving -- and taking the lock would make it refuse at the only
/// moment it is wanted. It never writes, never recovers and never replays the journal, so it
/// reports what is on disk rather than what the application would make of it.
///
/// Returns a process exit code: 0 clean, 1 problems found, 2 could not be read.
[[nodiscard]] int inspect_project(const std::filesystem::path& dir, bool verbose);

} // namespace munkicast
