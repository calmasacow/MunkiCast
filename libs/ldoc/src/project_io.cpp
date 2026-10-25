// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/project_io.hpp"

#include "ldoc/history.hpp"
#include "ldoc/journal.hpp"
#include "ldoc/json.hpp"
#include "ldoc/migrate.hpp"
#include "lcore/atomic_file.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ldoc {
namespace {

constexpr std::string_view kProjectFile = "project.json";
constexpr std::string_view kAssetsFile = "assets.json";
constexpr std::string_view kScenesDir = "scenes";
constexpr std::string_view kPrivateDir = ".libertas";
constexpr std::string_view kLockFile = "lock";
constexpr std::string_view kQuarantineDir = "quarantine";
constexpr std::string_view kStagingPrefix = "staging-";
constexpr std::string_view kJournalFile = "journal.jsonl";

std::string why(std::string_view what, const fs::path& p) {
    return std::format("{} {}: {}", what, p.string(), std::strerror(errno));
}

fs::path private_dir(const fs::path& dir) { return dir / kPrivateDir; }
fs::path scenes_dir(const fs::path& dir) { return dir / kScenesDir; }
fs::path staging_dir(const fs::path& dir, std::uint64_t gen) {
    return private_dir(dir) / std::format("{}{}", kStagingPrefix, gen);
}
fs::path scene_file(const fs::path& scenes, const Id& id) {
    return scenes / std::format("{}.json", id.to_string());
}

/// fsync a directory, so the renames and links inside it are durable. Skipping this is the
/// classic way to lose a file that was reported as saved.
void sync_dir(const fs::path& dir) {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)::fsync(fd);
    ::close(fd);
}

std::expected<void, std::string> write_sync(const fs::path& path, std::string_view data) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(why("open", path));
    const char* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const std::string msg = why("write", path);
            ::close(fd);
            return std::unexpected(msg);
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        const std::string msg = why("fsync", path);
        ::close(fd);
        return std::unexpected(msg);
    }
    if (::close(fd) != 0)
        return std::unexpected(why("close", path));
    return {};
}

/// Install one staged file at `to`, atomically, without emptying staging.
///
/// A hard link costs nothing and leaves the staged copy in place, which is what makes an
/// interrupted install resumable. Filesystems that cannot link fall back to a copy; the result
/// is the same, only slower.
std::expected<void, std::string> install(const fs::path& from, const fs::path& to) {
    fs::path tmp = to;
    tmp += ".installing";
    std::error_code ec;
    fs::remove(tmp, ec);

    if (::link(from.c_str(), tmp.c_str()) != 0) {
        if (errno != EPERM && errno != EXDEV && errno != EMLINK && errno != ENOSYS)
            return std::unexpected(why("link", from));
        fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return std::unexpected(
                std::format("copy {} -> {}: {}", from.string(), tmp.string(), ec.message()));
    }
    fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return std::unexpected(
            std::format("rename {} -> {}: {}", tmp.string(), to.string(), ec.message()));
    }
    return {};
}

bool call(const SaveHook& hook, SaveStep step) { return hook ? hook(step) : true; }

/// Every staging directory present, newest first.
std::vector<std::pair<std::uint64_t, fs::path>> staging_dirs(const fs::path& dir) {
    std::vector<std::pair<std::uint64_t, fs::path>> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(private_dir(dir), ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_directory(ec))
            continue;
        const std::string name = it->path().filename().string();
        if (!name.starts_with(kStagingPrefix))
            continue;
        std::uint64_t gen = 0;
        const std::string digits = name.substr(kStagingPrefix.size());
        if (digits.empty() ||
            !std::all_of(digits.begin(), digits.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; }))
            continue;
        try {
            gen = std::stoull(digits);
        } catch (...) {
            continue;
        }
        out.emplace_back(gen, it->path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return out;
}

/// Install everything in a staging directory, then tidy up. Used both by a normal save and by
/// recovery, so the two cannot drift apart.
std::expected<void, std::string> install_staging(const fs::path& dir, const fs::path& staging,
                                                 const SaveHook& hook) {
    const auto project_text = lc::read_file(staging / kProjectFile);
    if (!project_text)
        return std::unexpected(project_text.error());
    const auto parsed = project_from_json(*project_text);
    if (!parsed)
        return std::unexpected(
            std::format("the staged project could not be read: {}", parsed.error()));

    std::error_code ec;

    // Check that every staged file is there *before* installing any of them.
    //
    // Installing as we go and stopping at the first missing file is how a roll-forward turns
    // into the very thing it exists to prevent: some scenes from the new generation, the rest
    // from the old, and a project.json that agrees with neither. Staging is all-or-nothing, so
    // it has to be inspected all at once.
    for (const SceneRef& ref : parsed->scenes) {
        const fs::path from = scene_file(staging / kScenesDir, ref.id);
        if (!fs::exists(from, ec))
            return std::unexpected(
                std::format("the staged save is incomplete: {} is missing", from.string()));
    }

    fs::create_directories(scenes_dir(dir), ec);
    for (const SceneRef& ref : parsed->scenes) {
        const fs::path from = scene_file(staging / kScenesDir, ref.id);
        if (const auto r = install(from, scene_file(scenes_dir(dir), ref.id)); !r)
            return std::unexpected(r.error());
    }
    if (fs::exists(staging / kAssetsFile, ec)) {
        if (const auto r = install(staging / kAssetsFile, dir / kAssetsFile); !r)
            return std::unexpected(r.error());
    }
    sync_dir(scenes_dir(dir));
    sync_dir(dir);
    if (!call(hook, SaveStep::LeavesInstalled))
        return {};

    // The commit point.
    if (const auto r = install(staging / kProjectFile, dir / kProjectFile); !r)
        return std::unexpected(r.error());
    sync_dir(dir);
    if (!call(hook, SaveStep::RootInstalled))
        return {};

    // Only now is it safe to remove scene files the new project does not name: until this
    // moment, the project.json on disk might still have referred to them.
    std::vector<std::string> keep;
    keep.reserve(parsed->scenes.size());
    for (const SceneRef& ref : parsed->scenes)
        keep.push_back(std::format("{}.json", ref.id.to_string()));
    for (auto it = fs::directory_iterator(scenes_dir(dir), ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec)
            break;
        const std::string name = it->path().filename().string();
        if (!name.ends_with(".json"))
            continue;
        if (std::find(keep.begin(), keep.end(), name) == keep.end())
            fs::remove(it->path(), ec);
    }
    sync_dir(scenes_dir(dir));

    fs::remove_all(staging, ec);
    sync_dir(private_dir(dir));
    (void)call(hook, SaveStep::Done);
    return {};
}

} // namespace

// ---- lock -----------------------------------------------------------------------------------

ProjectLock::~ProjectLock() { release(); }

ProjectLock::ProjectLock(ProjectLock&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept {
    if (this != &other) {
        release();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void ProjectLock::release() {
    if (fd_ >= 0)
        ::close(fd_); // dropping the fd drops the lock
    fd_ = -1;
}

std::expected<ProjectLock, std::string> ProjectLock::acquire(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(private_dir(dir), ec);
    if (ec)
        return std::unexpected(
            std::format("create {}: {}", private_dir(dir).string(), ec.message()));

    const fs::path path = private_dir(dir) / kLockFile;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return std::unexpected(why("open", path));

    // An open-file-description lock, not a plain F_SETLK process lock.
    //
    // POSIX record locks are owned by the *process*, so a second open_project() in the same
    // process takes the lock again and both callers believe they hold it exclusively -- which is
    // exactly the case that matters for us, where the UI and a background importer live in one
    // process. OFD locks are owned by the file description, so two independent opens conflict
    // the way anyone would expect, and they are still released by the kernel when the holder
    // dies. Phase 3 wants flock() on macOS and LockFileEx() on Windows for the same property.
    struct flock fl{};
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    fl.l_pid = 0;
#ifdef F_OFD_SETLK
    const int lock_cmd = F_OFD_SETLK;
#else
    const int lock_cmd = F_SETLK;
#endif
    if (::fcntl(fd, lock_cmd, &fl) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EACCES || err == EAGAIN)
            return std::unexpected(
                std::format("{} is already open in another Libertas instance", dir.string()));
        return std::unexpected(std::format("lock {}: {}", path.string(), std::strerror(err)));
    }
    return ProjectLock{fd};
}

// ---- open -----------------------------------------------------------------------------------

std::expected<OpenResult, std::string> open_project(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return std::unexpected(std::format("{} is not a project directory", dir.string()));

    auto lock = ProjectLock::acquire(dir);
    if (!lock)
        return std::unexpected(lock.error());

    OpenResult result;
    result.lock = std::move(*lock);

    // Recovery first: an interrupted save left its staging directory behind, and a staging
    // generation newer than what project.json claims means the install never finished.
    const auto staged = staging_dirs(dir);
    for (const auto& [gen, path] : staged) {
        std::uint64_t committed = 0;
        if (const auto text = lc::read_file(dir / kProjectFile)) {
            if (const auto pf = project_from_json(*text))
                committed = pf->project.generation;
        }
        if (gen > committed) {
            const auto r = install_staging(dir, path, {});
            if (r) {
                result.recovered = true;
                result.problems.push_back(
                    Problem{Severity::Warning,
                            std::format("a save interrupted at generation {} was completed on "
                                        "open",
                                        gen),
                            Id::nil()});
            } else {
                // Incomplete staging: the interruption happened while writing, so nothing was
                // installed and the previous generation is intact. Discard it.
                fs::remove_all(path, ec);
                result.problems.push_back(
                    Problem{Severity::Warning,
                            std::format("an incomplete save at generation {} was discarded; the "
                                        "project opened at its previous state ({})",
                                        gen, r.error()),
                            Id::nil()});
            }
        } else {
            fs::remove_all(path, ec); // leftovers from a save that did finish
        }
    }

    const auto project_text = lc::read_file(dir / kProjectFile);
    if (!project_text)
        return std::unexpected(project_text.error());

    const auto file = project_from_json(*project_text);
    if (!file) {
        // A newer schema is the one failure that is not corruption, and it must not be written
        // back with the fields this build does not understand silently dropped.
        if (file.error().find("newer than this build") != std::string::npos) {
            result.read_only = true;
            result.problems.push_back(Problem{Severity::Error, file.error(), Id::nil()});
            return result;
        }
        return std::unexpected(std::format("{}: {}", (dir / kProjectFile).string(),
                                           file.error()));
    }
    result.project = file->project;

    if (const auto assets_text = lc::read_file(dir / kAssetsFile)) {
        const auto assets = assets_from_json(*assets_text);
        if (assets)
            result.project.assets = *assets;
        else
            result.problems.push_back(
                Problem{Severity::Error,
                        std::format("the asset manifest could not be read: {}", assets.error()),
                        Id::nil()});
    }

    for (const SceneRef& ref : file->scenes) {
        const fs::path path = scene_file(scenes_dir(dir), ref.id);
        const auto text = lc::read_file(path);
        if (!text) {
            result.problems.push_back(
                Problem{Severity::Error,
                        std::format("scene '{}' is missing its file ({}); saving will drop the "
                                    "reference",
                                    ref.name, path.string()),
                        ref.id});
            continue;
        }
        auto scene = scene_from_json(*text);
        if (!scene) {
            // Quarantine rather than refuse to open. Losing a whole collection to one bad byte
            // is the OBS failure this format exists to stop repeating, so the bytes are moved
            // somewhere safe, the operator is told exactly where, and every other scene opens.
            const fs::path quarantine = private_dir(dir) / kQuarantineDir;
            fs::create_directories(quarantine, ec);
            const fs::path to =
                quarantine / std::format("{}-gen{}.json", ref.id.to_string(), ref.generation);
            fs::rename(path, to, ec);
            result.problems.push_back(
                Problem{Severity::Error,
                        std::format("scene '{}' could not be read ({}); the file has been moved "
                                    "to {} and the scene is not loaded",
                                    ref.name, scene.error(), to.string()),
                        ref.id});
            continue;
        }
        if (scene->dropped_canvas_overrides > 0) {
            // ADR-0027 deleted per-canvas deltas along with the shared scenes they existed for.
            // Said out loud, with a count and a scene name, because silently discarding
            // authored work is how a format bump earns a reputation.
            result.problems.push_back(
                Problem{Severity::Warning,
                        std::format("scene '{}' had per-canvas overrides on {} source(s); the "
                                    "format no longer has them and they were dropped",
                                    ref.name, scene->dropped_canvas_overrides),
                        ref.id});
        }
        if (ref.generation != scene->generation) {
            result.problems.push_back(
                Problem{Severity::Warning,
                        std::format("scene '{}' was written by generation {} but the project "
                                    "expects {}",
                                    ref.name, scene->generation, ref.generation),
                        ref.id});
        }
        result.project.scenes.push_back(std::move(scene->scene));
    }

    // Only now, with the scenes loaded, can an older project be brought up to date: a version 1
    // scene has no canvas of its own and the answer depends on the canvas list.
    Migration migration = migrate(result.project, file->legacy_outputs);
    for (Problem& problem : migration.problems)
        result.problems.push_back(std::move(problem));

    // Replay anything the journal holds for exactly this generation. Entries tagged with an
    // older one are already in the saved project -- a journal left behind by a crash *after* a
    // save -- and replaying them would apply the same edits twice.
    const auto replayed = Journal::read(journal_path(dir));
    if (!replayed) {
        result.problems.push_back(
            Problem{Severity::Warning,
                    std::format("the command journal could not be read: {}", replayed.error()),
                    Id::nil()});
    } else {
        std::size_t applied = 0;
        std::size_t refused = 0;
        for (const Journal::Entry& e : replayed->entries) {
            if (e.base_generation != result.project.generation)
                continue;
            if (apply(result.project, e.command))
                ++applied;
            else
                ++refused;
        }
        result.recovered_commands = applied;
        if (applied > 0)
            result.problems.push_back(
                Problem{Severity::Warning,
                        std::format("{} unsaved change(s) were recovered from the journal",
                                    applied),
                        Id::nil()});
        if (refused > 0)
            result.problems.push_back(
                Problem{Severity::Error,
                        std::format("{} journal entr(ies) could not be replayed and were "
                                    "skipped",
                                    refused),
                        Id::nil()});
        if (replayed->truncated_tail)
            result.problems.push_back(
                Problem{Severity::Warning,
                        "the last journal entry was incomplete and was discarded; one change "
                        "made just before the interruption has been lost",
                        Id::nil()});
        if (replayed->unreadable > 0)
            result.problems.push_back(
                Problem{Severity::Error,
                        std::format("{} journal line(s) were unreadable", replayed->unreadable),
                        Id::nil()});
    }

    auto validation = validate(result.project);
    result.validation_problem_count = validation.size();
    for (Problem& p : validation)
        result.problems.push_back(std::move(p));
    return result;
}

// ---- save -----------------------------------------------------------------------------------

std::expected<void, std::string> save_project(Project& project, const fs::path& dir,
                                              const SaveHook& hook) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return std::unexpected(std::format("{} is not a project directory", dir.string()));

    const std::uint64_t gen = project.generation + 1;
    const fs::path staging = staging_dir(dir, gen);
    fs::remove_all(staging, ec);
    fs::create_directories(staging / kScenesDir, ec);
    if (ec)
        return std::unexpected(std::format("create {}: {}", staging.string(), ec.message()));

    Project staged = project;
    staged.generation = gen;

    for (const Scene& s : staged.scenes) {
        const auto r = write_sync(scene_file(staging / kScenesDir, s.id),
                                  scene_to_json(s, gen));
        if (!r)
            return std::unexpected(r.error());
    }
    if (const auto r = write_sync(staging / kAssetsFile, assets_to_json(staged.assets)); !r)
        return std::unexpected(r.error());
    if (const auto r = write_sync(staging / kProjectFile, project_to_json(staged)); !r)
        return std::unexpected(r.error());

    sync_dir(staging / kScenesDir);
    sync_dir(staging);
    sync_dir(private_dir(dir));

    if (!call(hook, SaveStep::Staged))
        return {}; // abandoned before anything was installed

    if (const auto r = install_staging(dir, staging, hook); !r)
        return std::unexpected(r.error());

    // Only report the new generation if project.json actually got installed.
    if (const auto text = lc::read_file(dir / kProjectFile)) {
        if (const auto pf = project_from_json(*text); pf && pf->project.generation == gen) {
            project.generation = gen;
            // Everything the journal held is now in the project, so it starts again from empty.
            // Clearing it before the commit would lose the work if the commit then failed.
            if (auto j = Journal::open(journal_path(dir)))
                (void)j->clear();
        }
    }
    return {};
}

std::filesystem::path journal_path(const fs::path& dir) {
    return private_dir(dir) / kJournalFile;
}

std::expected<void, std::string> create_project(const Project& project, const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(scenes_dir(dir), ec);
    if (ec)
        return std::unexpected(std::format("create {}: {}", dir.string(), ec.message()));
    fs::create_directories(private_dir(dir), ec);

    Project copy = project;
    copy.generation = 0;
    auto lock = ProjectLock::acquire(dir);
    if (!lock)
        return std::unexpected(lock.error());
    return save_project(copy, dir);
}

} // namespace ldoc
