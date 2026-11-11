// SPDX-FileCopyrightText: MunkiCast contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/command.hpp"
#include "ldoc/document.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The boundary between whoever is editing a document and whoever owns it.
///
/// ADR-0005 puts the document in `munkicast-session` and gives the UI **zero authority** over it.
/// That process split is not built yet, and this interface is what makes building it later a
/// change of transport rather than a rewrite: today `LocalSession` owns the document in the same
/// process, and tomorrow a proxy over `lipc` implements the same five operations by talking to
/// another one.
///
/// Three rules keep that promise, and they are rules about the *caller*, not the implementation:
///
/// 1. **Nothing outside here may hold a mutable Project.** Reading is a const reference;
///    writing is `run()`. A caller that reaches around this is a caller that stops compiling the
///    day the document moves to another process.
///
/// 2. **Update from the notification, never from the return value of `run()`.** `run()` returns
///    an error for the operator to read, nothing more. Over a socket it may one day return
///    before the command has been applied, and a UI that repaints on the return value would then
///    repaint too early. One that repaints on the notification does not change at all.
///
/// 3. **Errors are values.** Nothing throws across this boundary, because one day it is a
///    boundary exceptions cannot cross.
namespace lsession {

using ldoc::Command;
using ldoc::Problem;
using ldoc::Project;

struct Change {
    enum class Kind : std::uint8_t {
        /// A command was applied.
        Applied,
        /// A command was undone. `command` is the inverse that was actually applied.
        Undone,
        /// A command was redone.
        Redone,
        /// The whole document was replaced: opened, reverted, or recovered.
        Reset,
        /// The document was written to disk. Nothing in it changed.
        Saved,
    };

    Kind kind = Kind::Reset;
    /// What was actually applied to the document, for a listener that wants to update precisely
    /// rather than re-read everything. Absent for Reset and Saved.
    std::optional<Command> command;
    /// Bumps on every change, so a listener can tell whether it is up to date.
    std::uint64_t revision = 0;
};

/// Something worth telling the operator that is not an error from a call they made: a journal
/// that could not be written, a recovered save, a problem found on open.
using NoticeHandler = std::function<void(ldoc::Severity, const std::string&)>;
using ChangeListener = std::function<void(const Change&)>;

class Session {
public:
    virtual ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // ---- reading ----------------------------------------------------------------------------

    /// The current document. A const reference in both implementations: the remote one keeps a
    /// local replica that commands are applied to as they arrive, which is what a responsive UI
    /// needs anyway.
    [[nodiscard]] virtual const Project& project() const = 0;
    [[nodiscard]] virtual std::uint64_t revision() const = 0;

    /// Everything structurally wrong with the document right now, plus anything that went wrong
    /// opening it. Recomputed on demand.
    [[nodiscard]] virtual std::vector<Problem> problems() const = 0;

    // ---- writing ----------------------------------------------------------------------------

    /// Apply a command. Returns its description, or why it was refused.
    virtual std::expected<std::string, std::string> run(const Command& command) = 0;

    /// Apply a command as part of a continuous gesture -- a dragged fader, a dragged handle --
    /// so that the whole drag is one undo step landing back where the drag began. `gesture`
    /// names the control being dragged and must be unique to it; `end_gesture()` closes it when
    /// the operator lets go. See `ldoc::DocumentHistory::run` for what collapses and why.
    ///
    /// Each command is still notified and still journaled individually: a listener has to see
    /// every intermediate value or the fader it draws will not follow the drag, and the journal
    /// has to agree with what was actually applied. Coalescing is an undo-stack concern only.
    virtual std::expected<std::string, std::string> run(const Command& command,
                                                        std::string_view gesture) = 0;
    virtual void end_gesture() = 0;

    virtual std::expected<void, std::string> undo() = 0;
    virtual std::expected<void, std::string> redo() = 0;
    [[nodiscard]] virtual bool can_undo() const = 0;
    [[nodiscard]] virtual bool can_redo() const = 0;
    [[nodiscard]] virtual std::string undo_description() const = 0;
    [[nodiscard]] virtual std::string redo_description() const = 0;

    // ---- persistence ------------------------------------------------------------------------

    /// True when there are changes the journal is holding but the project file is not.
    [[nodiscard]] virtual bool dirty() const = 0;
    /// Where it lives, or empty for a document that has never been saved.
    [[nodiscard]] virtual std::filesystem::path path() const = 0;
    virtual std::expected<void, std::string> save() = 0;

    // ---- notification -----------------------------------------------------------------------

    /// Returns a token for `unsubscribe`. Listeners are called in the order they subscribed,
    /// after the change has been applied.
    virtual int subscribe(ChangeListener listener) = 0;
    virtual void unsubscribe(int token) = 0;
    virtual void set_notice_handler(NoticeHandler handler) = 0;

protected:
    Session() = default;
};

} // namespace lsession
