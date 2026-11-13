// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "lsession/session.hpp"

#include "ldoc/history.hpp"
#include "ldoc/journal.hpp"
#include "ldoc/project_io.hpp"

#include <map>
#include <memory>
#include <optional>
#include <thread>

namespace lsession {

/// A Session that owns the document in this process.
///
/// It holds the undo history, the journal and the project lock, so everything that has to happen
/// together when a command is applied -- apply it, record it durably, tell everyone -- happens in
/// one place. When the document moves to `munkicast-session`, this class moves with it and the UI
/// gets a proxy implementing the same interface.
class LocalSession final : public Session {
public:
    ~LocalSession() override;

    /// Open an existing project directory. Holds its lock until destroyed, replays any journal,
    /// and reports whatever was wrong through `problems()`.
    [[nodiscard]] static std::expected<std::unique_ptr<LocalSession>, std::string> open(
        const std::filesystem::path& dir);

    /// Create a new project directory from `project` and open it.
    [[nodiscard]] static std::expected<std::unique_ptr<LocalSession>, std::string> create(
        const Project& project, const std::filesystem::path& dir);

    /// A document with nowhere to live yet: no lock, no journal, and `save()` fails until it is
    /// given a path. For tests, and for a new project the operator has not chosen a home for.
    [[nodiscard]] static std::unique_ptr<LocalSession> in_memory(Project project);

    [[nodiscard]] const Project& project() const override;
    [[nodiscard]] std::uint64_t revision() const override { return revision_; }
    [[nodiscard]] std::vector<Problem> problems() const override;

    std::expected<std::string, std::string> run(const Command& command) override;
    std::expected<std::string, std::string> run(const Command& command,
                                                std::string_view gesture) override;
    void end_gesture() override;
    std::expected<void, std::string> undo() override;
    std::expected<void, std::string> redo() override;
    [[nodiscard]] bool can_undo() const override { return history_.can_undo(); }
    [[nodiscard]] bool can_redo() const override { return history_.can_redo(); }
    [[nodiscard]] std::string undo_description() const override;
    [[nodiscard]] std::string redo_description() const override;

    [[nodiscard]] bool dirty() const override { return dirty_; }
    [[nodiscard]] std::filesystem::path path() const override { return dir_; }
    std::expected<void, std::string> save() override;
    void save_async(Post post,
                    std::function<void(std::expected<void, std::string>)> done) override;
    void wait_for_save() override;

    /// Give an in-memory document a home. Fails if it already has one.
    std::expected<void, std::string> save_as(const std::filesystem::path& dir);

    int subscribe(ChangeListener listener) override;
    void unsubscribe(int token) override;
    void set_notice_handler(NoticeHandler handler) override;

private:
    explicit LocalSession(Project project);

    void notify(Change::Kind kind, std::optional<Command> command);
    void notice(ldoc::Severity severity, const std::string& text);
    /// Record a command in the journal, complaining rather than failing if it cannot be: the
    /// command has already been applied, and refusing it now would make the document and the
    /// operator's screen disagree.
    void journal(const Command& command);
    /// Snapshot for a save: the document without on-air state, and the journal rotated so that
    /// commands made while the save runs land in a fresh file.
    std::expected<Project, std::string> begin_save();
    /// Apply a finished save on the session's thread.
    void finish_save(const std::expected<std::uint64_t, std::string>& result,
                     std::uint64_t revision_at_snapshot);
    void complete_save();

    ldoc::DocumentHistory history_;
    ldoc::Journal journal_;
    ldoc::ProjectLock lock_;
    std::filesystem::path dir_;

    std::vector<Problem> open_problems_;
    std::map<int, ChangeListener> listeners_;
    NoticeHandler notice_handler_;
    int next_token_ = 1;
    std::uint64_t revision_ = 0;
    /// Revision of the last change that was journaled (an edit, not a take). A save clears
    /// `dirty_` only if no edit arrived after its snapshot.
    std::uint64_t edit_revision_ = 0;
    bool dirty_ = false;

    // Declared last so it is joined before the members a running save would touch.
    bool saving_ = false;
    bool save_again_ = false;
    std::uint64_t save_at_ = 0;
    std::shared_ptr<std::optional<std::expected<std::uint64_t, std::string>>> save_outcome_;
    std::function<void(std::expected<void, std::string>)> save_done_;
    Post again_post_;
    std::function<void(std::expected<void, std::string>)> again_done_;
    std::jthread saver_;
};

} // namespace lsession
