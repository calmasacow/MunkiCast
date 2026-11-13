// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

/// A project on disk.
///
/// ```
/// MyShow.munkicast/
///   project.json          canvases, buses, decks, sources, outputs, settings, scene manifest
///   assets.json           the asset manifest
///   scenes/<ulid>.json    one file per scene
///   .munkicast/
///     lock                an OS-held lock, not a pidfile
///     staging-<gen>/      the next generation, until it is committed
///     quarantine/         scene files that would not parse
/// ```
///
/// A directory rather than one file, for two reasons. It diffs, so a project can live in git and
/// a change can be reviewed. And **a corrupt scene costs one scene** -- losing a whole collection
/// to one bad byte is a real OBS failure mode and it must not be reproducible here.
namespace ldoc {

/// An exclusive lock on a project directory, held by the kernel.
///
/// `fcntl(F_SETLK)` rather than an `O_EXCL` pidfile: the kernel drops these when the holder dies,
/// however it dies. A pidfile goes stale on a crash and leaves people deleting lock files by
/// hand, which is a bug-report generator and teaches them to ignore the lock.
class ProjectLock {
public:
    ProjectLock() = default;
    ~ProjectLock();
    ProjectLock(ProjectLock&& other) noexcept;
    ProjectLock& operator=(ProjectLock&& other) noexcept;
    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;

    [[nodiscard]] static std::expected<ProjectLock, std::string> acquire(
        const std::filesystem::path& dir);

    [[nodiscard]] bool held() const { return fd_ >= 0; }
    void release();

private:
    explicit ProjectLock(int fd) : fd_(fd) {}
    int fd_ = -1;
};

struct OpenResult {
    Project project;
    /// Held for as long as the project is open.
    ProjectLock lock;
    /// What was wrong, including quarantined scenes and anything `validate` found. Opening
    /// succeeds with problems; it is the caller's job to show them.
    std::vector<Problem> problems;
    /// Trailing validation snapshot in `problems`. Sessions replace it with live validation;
    /// load/recovery diagnostics before it remain relevant for the lifetime of the session.
    std::size_t validation_problem_count = 0;
    /// The project's schema is newer than this build understands, so it was opened without
    /// parsing and must not be written back.
    bool read_only = false;
    /// An interrupted save was found and completed.
    bool recovered = false;
    /// Commands replayed from the journal: work done after the last save and not lost to the
    /// crash that followed it.
    std::size_t recovered_commands = 0;
};

/// Where the command journal for a project lives.
[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& dir);

/// Where a background save parks the journal it is folding in (see `Journal::rotate`). Replayed
/// before `journal_path`, so a crash during that save loses nothing.
[[nodiscard]] std::filesystem::path saving_journal_path(const std::filesystem::path& dir);

/// Program and preview are what is on air, not part of the show. They are never written to disk,
/// and every open starts each canvas on its emergency scene.
void clear_on_air(Project& project);
void start_on_emergency(Project& project);

[[nodiscard]] std::expected<OpenResult, std::string> open_project(
    const std::filesystem::path& dir);

/// Create a new project directory and write `project` into it.
[[nodiscard]] std::expected<void, std::string> create_project(
    const Project& project, const std::filesystem::path& dir);

/// The points a save can be interrupted at, in order. Exposed so the recovery path can be tested
/// by stopping at each one rather than by hoping.
enum class SaveStep : std::uint8_t {
    /// Everything is written into the staging directory and fsynced. Nothing is installed.
    Staged,
    /// Scene files and the asset manifest are installed. `project.json` is still the old one.
    LeavesInstalled,
    /// `project.json` is installed. This is the commit point.
    RootInstalled,
    /// Staging removed, scene files for deleted scenes removed.
    Done,
};

/// Called after each step. Returning false abandons the save there, leaving the directory exactly
/// as that step left it -- which is what a crash would do.
using SaveHook = std::function<bool(SaveStep)>;

/// Write `project` to `dir` as a new generation.
///
/// On success `project.generation` is the generation that was committed. On an abandoned save it
/// is left alone, because nothing was committed.
///
/// The protocol, and why each part of it is there:
///
///  1. Everything is written into `.munkicast/staging-<gen>/` and fsynced. Until this finishes,
///     the project directory is untouched.
///  2. Scene files and the asset manifest are **hard-linked** into place, not moved. Staging
///     stays complete, so an interruption part-way through installing can still be finished from
///     it. Moving them would leave staging half-empty at exactly the moment it is needed.
///  3. `project.json` is installed last and is the commit point: it carries the new generation
///     and can never name a file that is not already on disk.
///  4. Scene files for scenes that no longer exist are deleted *after* that, so the old
///     `project.json` never refers to a file that has already gone.
[[nodiscard]] std::expected<void, std::string> save_project(Project& project,
                                                            const std::filesystem::path& dir,
                                                            const SaveHook& hook = {});

} // namespace ldoc
